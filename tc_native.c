/* =============================================================================
 * Конвертер текстур - Texture Converter
 * Версия: 3.4
 * Автор: Antoñŷ
 * Файл: tc_native.c (нативное расширение Ruby, исходник; в rbz не входит)
 *
 * Задача: декодеры и энкодеры WEBP и AVIF для плагина SketchUp (а также
 * декодеры RAW и HEIC/HEIF по флагам сборки, в версии 3.4 не используются).
 * Декодирование: принимает байты файла (Ruby читает файл сам, поэтому пути с
 * кириллицей не проблема) и возвращает [ширина, высота, RGBA-строка].
 * Кодирование: принимает сырые пиксели ImageRep (8 бит серый, 24 или 32 бита,
 * BGR или RGB, со строкой stride; серый расширяется в RGB внутри C) и возвращает байты готового файла; Ruby пишет файл сам.
 * Запись PNG для декодеров делает Ruby (lib/png_writer.rb).
 *
 * Флаги сборки (определяются в CMakeLists.txt): TC_WITH_WEBP, TC_WITH_AVIF,
 * TC_WITH_RAW, TC_WITH_HEIF. Кодировка исходника UTF-8 (для MSVC: /utf-8).
 * ========================================================================== */
#include <ruby.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TC_WITH_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#endif
#ifdef TC_WITH_AVIF
#include <avif/avif.h>
#endif
#ifdef TC_WITH_RAW
#include <libraw/libraw.h>
#endif
#ifdef TC_WITH_HEIF
#include <libheif/heif.h>
#endif

#define TC_ABI 3

/* Вне сборки самого Ruby RUBY_FUNC_EXPORTED пустой (проверено в заголовках
 * SketchUp Ruby 3.2), поэтому под MSVC экспорт Init_tc_native задаём сами. */
#ifdef _MSC_VER
#define TC_EXPORT __declspec(dllexport)
#else
#define TC_EXPORT RUBY_FUNC_EXPORTED
#endif
/* AVIF: скорость кодирования (0 медленно и плотно, 10 быстро) и число потоков */
#define TC_AVIF_SPEED   6
#define TC_AVIF_THREADS 4
/* Верхний предел размера RGBA-буфера: 1.5 ГБ (long на Windows 32-битный) */
#define TC_MAX_BYTES ((size_t)1536 * 1024 * 1024)

typedef struct tc_img {
  uint8_t *pixels;            /* RGBA, строки сверху вниз, без выравнивания */
  size_t   len;
  int      w, h;
  void   (*release)(struct tc_img *);
  char     err[256];
} tc_img;

static void tc_seterr(tc_img *o, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(o->err, sizeof(o->err), fmt, ap);
  va_end(ap);
}

/* Проверка размера и заполнение результата, общая для всех декодеров */
static int tc_check_size(tc_img *o, int w, int h) {
  if (w <= 0 || h <= 0) {
    tc_seterr(o, "неверный размер изображения %dx%d", w, h);
    return 0;
  }
  if ((size_t)w > TC_MAX_BYTES / 4 / (size_t)h) {
    tc_seterr(o, "изображение %dx%d слишком большое", w, h);
    return 0;
  }
  return 1;
}

static void rel_free(tc_img *i) { free(i->pixels); i->pixels = NULL; }

/* ---------------------------------------------------------------- WEBP --- */
#ifdef TC_WITH_WEBP
static void rel_webp(tc_img *i) { WebPFree(i->pixels); i->pixels = NULL; }

static int dec_webp(const uint8_t *d, size_t n, tc_img *o) {
  int w = 0, h = 0;
  if (!WebPGetInfo(d, n, &w, &h)) {
    tc_seterr(o, "не WebP или файл повреждён");
    return 0;
  }
  if (!tc_check_size(o, w, h)) return 0;
  uint8_t *px = WebPDecodeRGBA(d, n, &w, &h);
  if (!px) {
    tc_seterr(o, "libwebp не смогла декодировать файл");
    return 0;
  }
  o->pixels = px; o->w = w; o->h = h; o->len = (size_t)w * h * 4;
  o->release = rel_webp;
  return 1;
}
#endif

/* ---------------------------------------------------------------- AVIF --- */
#ifdef TC_WITH_AVIF
static int dec_avif(const uint8_t *d, size_t n, tc_img *o) {
  avifDecoder *dec = avifDecoderCreate();
  if (!dec) { tc_seterr(o, "avifDecoderCreate вернул NULL"); return 0; }
  avifResult r = avifDecoderSetIOMemory(dec, d, n);
  if (r == AVIF_RESULT_OK) r = avifDecoderParse(dec);
  if (r == AVIF_RESULT_OK) r = avifDecoderNextImage(dec);
  if (r != AVIF_RESULT_OK) {
    tc_seterr(o, "libavif: %s", avifResultToString(r));
    avifDecoderDestroy(dec);
    return 0;
  }
  int w = (int)dec->image->width, h = (int)dec->image->height;
  if (!tc_check_size(o, w, h)) { avifDecoderDestroy(dec); return 0; }

  avifRGBImage rgb;
  memset(&rgb, 0, sizeof(rgb));
  avifRGBImageSetDefaults(&rgb, dec->image);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth  = 8;
  r = avifRGBImageAllocatePixels(&rgb);
  if (r == AVIF_RESULT_OK) r = avifImageYUVToRGB(dec->image, &rgb);
  if (r != AVIF_RESULT_OK) {
    tc_seterr(o, "libavif (преобразование в RGB): %s", avifResultToString(r));
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(dec);
    return 0;
  }
  /* копия без выравнивания строк, чтобы не зависеть от rowBytes */
  uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
  if (!px) {
    tc_seterr(o, "не хватает памяти (%dx%d)", w, h);
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(dec);
    return 0;
  }
  for (int y = 0; y < h; y++)
    memcpy(px + (size_t)y * w * 4, rgb.pixels + (size_t)y * rgb.rowBytes, (size_t)w * 4);
  avifRGBImageFreePixels(&rgb);
  avifDecoderDestroy(dec);
  o->pixels = px; o->w = w; o->h = h; o->len = (size_t)w * h * 4;
  o->release = rel_free;
  return 1;
}
#endif

/* ------------------------------------------------------------ HEIC/HEIF --- */
#ifdef TC_WITH_HEIF
static int dec_heif(const uint8_t *d, size_t n, tc_img *o) {
  heif_context *ctx = heif_context_alloc();
  if (!ctx) { tc_seterr(o, "heif_context_alloc вернул NULL"); return 0; }
  heif_image_handle *hdl = NULL;
  heif_image *img = NULL;
  struct heif_error e = heif_context_read_from_memory_without_copy(ctx, d, n, NULL);
  if (e.code == heif_error_Ok) e = heif_context_get_primary_image_handle(ctx, &hdl);
  if (e.code == heif_error_Ok)
    e = heif_decode_image(hdl, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, NULL);
  if (e.code != heif_error_Ok) {
    tc_seterr(o, "libheif: %s", e.message ? e.message : "ошибка");
    if (hdl) heif_image_handle_release(hdl);
    heif_context_free(ctx);
    return 0;
  }
  int w = heif_image_get_width(img, heif_channel_interleaved);
  int h = heif_image_get_height(img, heif_channel_interleaved);
  int stride = 0;
  const uint8_t *src = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
  int ok = src && tc_check_size(o, w, h);
  uint8_t *px = NULL;
  if (ok) {
    px = (uint8_t *)malloc((size_t)w * h * 4);
    if (!px) { tc_seterr(o, "не хватает памяти (%dx%d)", w, h); ok = 0; }
  } else if (!src) {
    tc_seterr(o, "libheif не отдала плоскость изображения");
  }
  if (ok)
    for (int y = 0; y < h; y++)
      memcpy(px + (size_t)y * w * 4, src + (size_t)y * stride, (size_t)w * 4);
  heif_image_release(img);
  heif_image_handle_release(hdl);
  heif_context_free(ctx);
  if (!ok) return 0;
  o->pixels = px; o->w = w; o->h = h; o->len = (size_t)w * h * 4;
  o->release = rel_free;
  return 1;
}
#endif

/* ----------------------------------------------------------------- RAW --- */
#ifdef TC_WITH_RAW
static int dec_raw(const uint8_t *d, size_t n, tc_img *o) {
  libraw_data_t *lr = libraw_init(0);
  if (!lr) { tc_seterr(o, "libraw_init вернул NULL"); return 0; }
  lr->params.output_bps    = 8;   /* 8 бит на канал */
  lr->params.output_color  = 1;   /* sRGB */
  lr->params.use_camera_wb = 1;   /* баланс белого из файла камеры */
  int rc = libraw_open_buffer(lr, d, n);
  if (rc == LIBRAW_SUCCESS) rc = libraw_unpack(lr);
  if (rc == LIBRAW_SUCCESS) rc = libraw_dcraw_process(lr);
  if (rc != LIBRAW_SUCCESS) {
    tc_seterr(o, "LibRaw: %s", libraw_strerror(rc));
    libraw_close(lr);
    return 0;
  }
  int err = 0;
  libraw_processed_image_t *im = libraw_dcraw_make_mem_image(lr, &err);
  if (!im) {
    tc_seterr(o, "LibRaw (make_mem_image): %s", libraw_strerror(err));
    libraw_close(lr);
    return 0;
  }
  int w = im->width, h = im->height, ok = 1;
  uint8_t *px = NULL;
  if (im->type != LIBRAW_IMAGE_BITMAP || im->bits != 8 || im->colors != 3) {
    tc_seterr(o, "LibRaw вернула неожиданный формат (type=%d bits=%d colors=%d)",
              (int)im->type, (int)im->bits, (int)im->colors);
    ok = 0;
  } else if (!tc_check_size(o, w, h)) {
    ok = 0;
  } else if (!(px = (uint8_t *)malloc((size_t)w * h * 4))) {
    tc_seterr(o, "не хватает памяти (%dx%d)", w, h);
    ok = 0;
  }
  if (ok) {
    const uint8_t *s = im->data;
    uint8_t *t = px;
    size_t cnt = (size_t)w * h;
    for (size_t i = 0; i < cnt; i++) {
      t[0] = s[0]; t[1] = s[1]; t[2] = s[2]; t[3] = 255;
      s += 3; t += 4;
    }
  }
  libraw_dcraw_clear_mem(im);
  libraw_close(lr);
  if (!ok) return 0;
  o->pixels = px; o->w = w; o->h = h; o->len = (size_t)w * h * 4;
  o->release = rel_free;
  return 1;
}
#endif

/* -------------------------------------------------------- таблица форматов */
typedef int (*tc_dec_fn)(const uint8_t *, size_t, tc_img *);
typedef struct { const char *id; tc_dec_fn fn; } tc_codec;

static const tc_codec CODECS[] = {
#ifdef TC_WITH_WEBP
  { "webp", dec_webp },
#endif
#ifdef TC_WITH_AVIF
  { "avif", dec_avif },
#endif
#ifdef TC_WITH_HEIF
  { "heif", dec_heif },
#endif
#ifdef TC_WITH_RAW
  { "raw", dec_raw },
#endif
  { NULL, NULL }
};

/* --------------------------------------------------------------- ENCODE --- */
typedef struct tc_blob {
  uint8_t *data;              /* готовый файл */
  size_t   len;
  char     err[256];
} tc_blob;

static void tb_seterr(tc_blob *o, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(o->err, sizeof(o->err), fmt, ap);
  va_end(ap);
}

/* Копия в буфер malloc, чтобы освобождение было одинаковым для всех кодеков */
static int tb_take(tc_blob *o, const uint8_t *d, size_t n) {
  o->data = (uint8_t *)malloc(n ? n : 1);
  if (!o->data) { tb_seterr(o, "не хватает памяти под результат (%zu байт)", n); return 0; }
  memcpy(o->data, d, n);
  o->len = n;
  return 1;
}

/* rgba: w*h*4 без выравнивания; has_alpha = 0, если все альфа равны 255 */
typedef int (*tc_enc_fn)(const uint8_t *rgba, int w, int h, int has_alpha, int quality, tc_blob *o);

#ifdef TC_WITH_WEBP
static int enc_webp(const uint8_t *rgba, int w, int h, int has_alpha, int quality, tc_blob *o) {
  (void)has_alpha;   /* libwebp сам не пишет канал альфа, если он весь 255 */
  if (w > WEBP_MAX_DIMENSION || h > WEBP_MAX_DIMENSION) {
    tb_seterr(o, "размер %dx%d больше предела WebP (%d)", w, h, WEBP_MAX_DIMENSION);
    return 0;
  }
  uint8_t *out = NULL;
  size_t n = WebPEncodeRGBA(rgba, w, h, w * 4, (float)quality, &out);
  if (n == 0 || !out) { tb_seterr(o, "WebPEncodeRGBA не создал данные"); return 0; }
  int ok = tb_take(o, out, n);
  WebPFree(out);
  return ok;
}
#endif

#ifdef TC_WITH_AVIF
static int avif_can_encode(void) {
  return avifCodecName(AVIF_CODEC_CHOICE_AUTO, AVIF_CODEC_FLAG_CAN_ENCODE) != NULL;
}

static int enc_avif(const uint8_t *rgba, int w, int h, int has_alpha, int quality, tc_blob *o) {
  if (!avif_can_encode()) {
    tb_seterr(o, "libavif собран без энкодера (нужна фича aom в vcpkg.json)");
    return 0;
  }
  avifImage *im = avifImageCreate((uint32_t)w, (uint32_t)h, 8, AVIF_PIXEL_FORMAT_YUV444);
  if (!im) { tb_seterr(o, "avifImageCreate вернул NULL (%dx%d)", w, h); return 0; }

  avifRGBImage rgb;
  memset(&rgb, 0, sizeof(rgb));
  avifRGBImageSetDefaults(&rgb, im);
  rgb.format      = AVIF_RGB_FORMAT_RGBA;
  rgb.depth       = 8;
  rgb.pixels      = (uint8_t *)rgba;   /* только чтение, копию делает libavif */
  rgb.rowBytes    = (uint32_t)w * 4;
  rgb.ignoreAlpha = has_alpha ? AVIF_FALSE : AVIF_TRUE;

  avifResult r = avifImageRGBToYUV(im, &rgb);
  if (r != AVIF_RESULT_OK) {
    tb_seterr(o, "libavif (RGB в YUV): %s", avifResultToString(r));
    avifImageDestroy(im);
    return 0;
  }
  avifEncoder *e = avifEncoderCreate();
  if (!e) { tb_seterr(o, "avifEncoderCreate вернул NULL"); avifImageDestroy(im); return 0; }
  e->quality      = quality;
  e->qualityAlpha = quality;
  e->speed        = TC_AVIF_SPEED;
  e->maxThreads   = TC_AVIF_THREADS;

  avifRWData raw = AVIF_DATA_EMPTY;
  r = avifEncoderAddImage(e, im, 1, AVIF_ADD_IMAGE_FLAG_SINGLE);
  if (r == AVIF_RESULT_OK) r = avifEncoderFinish(e, &raw);
  int ok = 0;
  if (r != AVIF_RESULT_OK) tb_seterr(o, "libavif (кодирование): %s", avifResultToString(r));
  else ok = tb_take(o, raw.data, raw.size);
  avifRWDataFree(&raw);
  avifEncoderDestroy(e);
  avifImageDestroy(im);
  return ok;
}
#endif

typedef struct { const char *id; tc_enc_fn fn; int (*can)(void); } tc_encoder;

static const tc_encoder ENCODERS[] = {
#ifdef TC_WITH_WEBP
  { "webp", enc_webp, NULL },
#endif
#ifdef TC_WITH_AVIF
  { "avif", enc_avif, avif_can_encode },
#endif
  { NULL, NULL, NULL }
};

static VALUE blob_to_str(VALUE p) {
  tc_blob *b = (tc_blob *)p;
  return rb_str_new((const char *)b->data, (long)b->len);
}

static VALUE blob_release(VALUE p) {
  tc_blob *b = (tc_blob *)p;
  free(b->data);
  b->data = NULL;
  return Qnil;
}

/* ------------------------------------------------------------- Ruby API --- */
static VALUE img_to_ary(VALUE p) {
  tc_img *i = (tc_img *)p;
  VALUE s = rb_str_new((const char *)i->pixels, (long)i->len);
  return rb_ary_new3(3, INT2NUM(i->w), INT2NUM(i->h), s);
}

static VALUE img_release(VALUE p) {
  tc_img *i = (tc_img *)p;
  if (i->release && i->pixels) i->release(i);
  return Qnil;
}

/* NativeCodecs.decode(id, bytes) -> [width, height, rgba_string] */
static VALUE tc_decode(VALUE self, VALUE id, VALUE data) {
  (void)self;
  StringValue(id);
  StringValue(data);
  const char *cid = StringValueCStr(id);
  const tc_codec *c = NULL;
  for (const tc_codec *k = CODECS; k->id; k++)
    if (strcmp(k->id, cid) == 0) { c = k; break; }
  if (!c) rb_raise(rb_eArgError, "формат '%s' не собран в нативный модуль", cid);
  if (RSTRING_LEN(data) <= 0) rb_raise(rb_eArgError, "%s: пустой файл", cid);

  tc_img img;
  memset(&img, 0, sizeof(img));
  int ok = c->fn((const uint8_t *)RSTRING_PTR(data), (size_t)RSTRING_LEN(data), &img);
  RB_GC_GUARD(data);
  if (!ok) rb_raise(rb_eRuntimeError, "%s: %s", cid, img.err);
  if (img.len > (size_t)LONG_MAX) {
    img_release((VALUE)&img);
    rb_raise(rb_eRuntimeError, "%s: результат слишком большой", cid);
  }
  return rb_ensure(img_to_ary, (VALUE)&img, img_release, (VALUE)&img);
}

/* NativeCodecs.encode(id, width, height, bits_per_pixel, stride, order, data, quality)
 *   -> String (байты файла)
 * bits_per_pixel: 8 (серый, один канал, расширяется в RGB, альфа 255), 24 или 32;
 * stride: байт на строку вместе с выравниванием
 * (Ruby считает data.bytesize / height); order: "bgr" (Windows) или "rgb"
 * (macOS), порядок цветовых каналов, альфа при 32 битах идёт четвёртой;
 * quality: 0..100. */
static VALUE tc_encode(VALUE self, VALUE id, VALUE vw, VALUE vh, VALUE vbpp, VALUE vstride,
                       VALUE order, VALUE data, VALUE vq) {
  (void)self;
  StringValue(id);
  StringValue(order);
  StringValue(data);
  const char *cid = StringValueCStr(id);
  const tc_encoder *c = NULL;
  for (const tc_encoder *k = ENCODERS; k->id; k++)
    if (strcmp(k->id, cid) == 0) { c = k; break; }
  if (!c) rb_raise(rb_eArgError, "формат '%s' не собран для записи", cid);
  if (c->can && !c->can()) rb_raise(rb_eRuntimeError, "%s: энкодер не собран в библиотеку", cid);

  int w = NUM2INT(vw), h = NUM2INT(vh), bpp = NUM2INT(vbpp), q = NUM2INT(vq);
  long stride = NUM2LONG(vstride);
  if (w <= 0 || h <= 0) rb_raise(rb_eArgError, "%s: неверный размер %dx%d", cid, w, h);
  if (bpp != 8 && bpp != 24 && bpp != 32) rb_raise(rb_eArgError, "%s: bits_per_pixel должен быть 8, 24 или 32, получено %d", cid, bpp);
  if (q < 0 || q > 100) rb_raise(rb_eArgError, "%s: quality вне диапазона 0..100 (%d)", cid, q);
  const char *ord = StringValueCStr(order);
  int ri, bi;
  if (strcmp(ord, "rgb") == 0)      { ri = 0; bi = 2; }
  else if (strcmp(ord, "bgr") == 0) { ri = 2; bi = 0; }
  else rb_raise(rb_eArgError, "%s: order должен быть \"rgb\" или \"bgr\", получено \"%s\"", cid, ord);
  int bytespp = bpp / 8;
  if (stride < (long)w * bytespp) rb_raise(rb_eArgError, "%s: stride %ld меньше ширины строки %d", cid, stride, w * bytespp);
  if ((size_t)w > TC_MAX_BYTES / 4 / (size_t)h) rb_raise(rb_eRuntimeError, "%s: изображение %dx%d слишком большое", cid, w, h);
  size_t need = (size_t)stride * (size_t)(h - 1) + (size_t)w * bytespp;
  if ((size_t)RSTRING_LEN(data) < need)
    rb_raise(rb_eArgError, "%s: данных %ld байт, нужно не меньше %zu (%dx%d, %d бит, stride %ld)",
             cid, RSTRING_LEN(data), need, w, h, bpp, stride);

  uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
  if (!rgba) rb_raise(rb_eNoMemError, "%s: не хватает памяти (%dx%d)", cid, w, h);
  const uint8_t *src = (const uint8_t *)RSTRING_PTR(data);
  int has_alpha = 0;
  for (int y = 0; y < h; y++) {
    const uint8_t *sp = src + (size_t)y * (size_t)stride;
    uint8_t *dp = rgba + (size_t)y * w * 4;
    for (int x = 0; x < w; x++, sp += bytespp, dp += 4) {
      if (bytespp == 1) {            /* серый 8 бит: R = G = B, альфа 255 */
        dp[0] = dp[1] = dp[2] = sp[0];
        dp[3] = 255;
        continue;
      }
      dp[0] = sp[ri]; dp[1] = sp[1]; dp[2] = sp[bi];
      dp[3] = bytespp == 4 ? sp[3] : 255;
      if (dp[3] != 255) has_alpha = 1;
    }
  }
  RB_GC_GUARD(data);

  tc_blob blob;
  memset(&blob, 0, sizeof(blob));
  int ok = c->fn(rgba, w, h, has_alpha, q, &blob);
  free(rgba);
  if (!ok) rb_raise(rb_eRuntimeError, "%s: %s", cid, blob.err);
  if (blob.len > (size_t)LONG_MAX) {
    blob_release((VALUE)&blob);
    rb_raise(rb_eRuntimeError, "%s: результат слишком большой", cid);
  }
  return rb_ensure(blob_to_str, (VALUE)&blob, blob_release, (VALUE)&blob);
}

/* NativeCodecs.encoders -> ["webp", "avif"] (что реально умеет писать) */
static VALUE tc_encoders(VALUE self) {
  (void)self;
  VALUE a = rb_ary_new();
  for (const tc_encoder *k = ENCODERS; k->id; k++)
    if (!k->can || k->can()) rb_ary_push(a, rb_str_new_cstr(k->id));
  return a;
}

/* NativeCodecs.features -> ["webp", "avif", ...] (что реально умеет читать) */
static VALUE tc_features(VALUE self) {
  (void)self;
  VALUE a = rb_ary_new();
  for (const tc_codec *k = CODECS; k->id; k++) rb_ary_push(a, rb_str_new_cstr(k->id));
  return a;
}

/* NativeCodecs.versions -> {"webp" => "1.6.0", ...} (для лога) */
static VALUE tc_versions(VALUE self) {
  (void)self;
  VALUE h = rb_hash_new();
#ifdef TC_WITH_WEBP
  {
    int v = WebPGetDecoderVersion();
    char b[32];
    snprintf(b, sizeof(b), "%d.%d.%d", (v >> 16) & 255, (v >> 8) & 255, v & 255);
    rb_hash_aset(h, rb_str_new_cstr("webp"), rb_str_new_cstr(b));
  }
#endif
#ifdef TC_WITH_AVIF
  rb_hash_aset(h, rb_str_new_cstr("avif"), rb_str_new_cstr(avifVersion()));
  {
    char cv[256];
    avifCodecVersions(cv);
    rb_hash_aset(h, rb_str_new_cstr("avif_codecs"), rb_str_new_cstr(cv));
  }
#endif
#ifdef TC_WITH_HEIF
  rb_hash_aset(h, rb_str_new_cstr("heif"), rb_str_new_cstr(heif_get_version()));
#endif
#ifdef TC_WITH_RAW
  rb_hash_aset(h, rb_str_new_cstr("raw"), rb_str_new_cstr(libraw_version()));
#endif
  return h;
}

TC_EXPORT void Init_tc_native(void) {
  VALUE root = rb_define_module("TextureConverter");
  VALUE m    = rb_define_module_under(root, "NativeCodecs");
  rb_define_const(m, "ABI", INT2FIX(TC_ABI));
  rb_define_module_function(m, "decode",   tc_decode,   2);
  rb_define_module_function(m, "encode",   tc_encode,   8);
  rb_define_module_function(m, "encoders", tc_encoders, 0);
  rb_define_module_function(m, "features", tc_features, 0);
  rb_define_module_function(m, "versions", tc_versions, 0);
#ifdef TC_WITH_HEIF
  heif_init(NULL);
#endif
}
