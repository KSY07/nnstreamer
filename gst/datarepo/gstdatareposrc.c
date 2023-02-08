/* SPDX-License-Identifier: LGPL-2.1-only */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd.
 *
 * @file	gstdatareposrc.c
 * @date	31 January 2023
 * @brief	GStreamer plugin to read file in MLOps Data repository into buffers
 * @see		https://github.com/nnstreamer/nnstreamer
 * @author	Hyunil Park <hyunil46.park@samsung.com>
 * @bug		No known bugs except for NYI items
 *
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 datareposrc location=mnist_trainingSet.dat ! \
 * other/tensors, format=static, num_tensors=2, framerate=0/1, \
 * dimensions=1:1:784:1.1:1:10:1, types=float32.float32 ! tensor_sink
 * ]|
 * |[
 * gst-launch-1.0 datareposrc location=image_%02ld.png ! pngdec ! fakesink
 * gst-launch-1.0 datareposrc location=audiofile ! audio/x-raw, format=S8, rate=48000, channels=2 ! fakesink
 * gst-launch-1.0 datareposrc location=videofile ! video/x-raw, format=RGB, width=320, height=240 ! fakesink
 * ]|
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <gst/gst.h>
#include <gst/video/video-info.h>
#include <gst/audio/audio-info.h>
#include <glib/gstdio.h>
#include <nnstreamer_plugin_api.h>
#include <nnstreamer_util.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "gstdatareposrc.h"

#define struct_stat struct stat
#ifndef S_ISREG
/* regular file */
#define S_ISREG(mode) ((mode)&_S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) ((mode)&_S_IFDIR)
#endif
/* socket */
#ifndef S_ISSOCK
#define S_ISSOCK(x) (0)
#endif
#ifndef O_BINARY
#define O_BINARY (0)
#endif

/**
 * @brief Default blocksize for reading
 */
#define DEFAULT_BLOCKSIZE       4*1024

/**
 * @brief Tensors caps
 */
#define TENSOR_CAPS GST_TENSORS_CAP_MAKE ("{ static, flexible }")
/**
 * @brief Video caps
 */
#define SUPPORTED_VIDEO_FORMAT \
  "{RGB, BGR, RGBx, BGRx, xRGB, xBGR, RGBA, BGRA, ARGB, ABGR, GRAY8}"
#define VIDEO_CAPS GST_VIDEO_CAPS_MAKE (SUPPORTED_VIDEO_FORMAT) "," \
  "interlace-mode = (string) progressive"
/**
 * @brief Audio caps
 */
#define SUPPORTED_AUDIO_FORMAT \
  "{S8, U8, S16LE, S16BE, U16LE, U16BE, S32LE, S32BE, U32LE, U32BE, F32LE, F32BE, F64LE, F64BE}"
#define AUDIO_CAPS GST_AUDIO_CAPS_MAKE (SUPPORTED_AUDIO_FORMAT) "," \
  "layout = (string) interleaved"
/**
 * @brief Text caps
 */
#define TEXT_CAPS "text/x-raw, format = (string) utf8"
/**
 * @brief Octet caps
 */
#define OCTET_CAPS "application/octet-stream"

/**
 * @brief Image caps
 */
#define IMAGE_CAPS \
  "image/png, width = (int) [ 16, 1000000 ], height = (int) [ 16, 1000000 ], framerate = (fraction) [ 0/1, MAX];" \
  "image/jpeg, width = (int) [ 16, 65535 ], height = (int) [ 16, 65535 ], framerate = (fraction) [ 0/1, MAX], sof-marker = (int) { 0, 1, 2, 4, 9 };" \
  "image/tiff, endianness = (int) { BIG_ENDIAN, LITTLE_ENDIAN };" \
  "image/gif"

static GstStaticPadTemplate srctemplate =
    GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (TENSOR_CAPS ";" VIDEO_CAPS ";" AUDIO_CAPS ";" IMAGE_CAPS
        ";" TEXT_CAPS ";" OCTET_CAPS));


GST_DEBUG_CATEGORY_STATIC (gst_data_repo_src_debug);
#define GST_CAT_DEFAULT gst_data_repo_src_debug

/* RepoSrc signals and args */
enum
{
  PROP_0,
  PROP_LOCATION,
};

static void gst_data_repo_src_finalize (GObject * object);
static void gst_data_repo_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_data_repo_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_data_repo_src_start (GstBaseSrc * basesrc);
static gboolean gst_data_repo_src_stop (GstBaseSrc * basesrc);
static GstCaps *gst_data_repo_src_get_caps (GstBaseSrc * basesrc,
    GstCaps * filter);
static gboolean gst_data_repo_src_set_caps (GstBaseSrc * basesrc,
    GstCaps * caps);
static GstFlowReturn gst_data_repo_src_create (GstPushSrc * pushsrc,
    GstBuffer ** buffer);

#define _do_init \
  GST_DEBUG_CATEGORY_INIT (gst_data_repo_src_debug, "datareposrc", 0, "datareposrc element");

#define gst_data_repo_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstDataRepoSrc, gst_data_repo_src, GST_TYPE_PUSH_SRC,
    _do_init);

/**
 * @brief initialize the datareposrc's class
 */
static void
gst_data_repo_src_class_init (GstDataRepoSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_data_repo_src_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_data_repo_src_get_property);

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "File Location",
          "Location of the file to read that is stored in MLOps Data Repository"
          "If the files are image, create pattern name like 'filename%04d.png'",
          NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  gobject_class->finalize = gst_data_repo_src_finalize;

  gst_element_class_set_static_metadata (gstelement_class,
      "NNStreamer MLOps Data Repository Source",
      "Source/File",
      "Read files in MLOps Data Repository into buffers",
      "Samsung Electronics Co., Ltd.");
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_data_repo_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_data_repo_src_stop);
  gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_data_repo_src_get_caps);
  gstbasesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_data_repo_src_set_caps);
  gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_data_repo_src_create);

  if (sizeof (off_t) < 8) {
    GST_LOG ("No large file support, sizeof (off_t) = %" G_GSIZE_FORMAT "!",
        sizeof (off_t));
  }
}

/**
 * @brief Initialize datareposrc
 */
static void
gst_data_repo_src_init (GstDataRepoSrc * src)
{
  src->filename = NULL;
  src->fd = 0;
  src->media_type = _NNS_TENSOR;
  src->offset = 0;
  src->successful_read = FALSE;

/* let's set value by property */
  src->frame_index = 0;
  src->start_frame_index = 0;
  src->stop_frame_index = 0;
}

/**
 * @brief Function to finalize instance.
 */
static void
gst_data_repo_src_finalize (GObject * object)
{
  GstDataRepoSrc *src = GST_DATA_REPO_SRC (object);

  g_free (src->filename);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/**
 * @brief Function to set file path.
 */
static gboolean
gst_data_repo_src_set_location (GstDataRepoSrc * src, const gchar * location,
    GError ** err)
{
  GstState state;

  /* the element must be stopped in order to do this */
  GST_OBJECT_LOCK (src);
  state = GST_STATE (src);
  if (state != GST_STATE_READY && state != GST_STATE_NULL)
    goto wrong_state;
  GST_OBJECT_UNLOCK (src);

  g_free (src->filename);

  /* clear the filename if we get a NULL */
  if (location == NULL) {
    src->filename = NULL;
  } else {
    /* should be UTF8 */
    src->filename = g_strdup (location);
    GST_INFO ("filename : %s", src->filename);
  }
  g_object_notify (G_OBJECT (src), "location");

  return TRUE;

  /* ERROR */
wrong_state:
  {
    g_warning ("Changing the `location' property on repo_src when a file is "
        "open is not supported.");
    if (err)
      g_set_error (err, GST_URI_ERROR, GST_URI_ERROR_BAD_STATE,
          "Changing the `location' property on repo_src when a file is "
          "open is not supported.");
    GST_OBJECT_UNLOCK (src);
    return FALSE;
  }
}

/**
 * @brief Setter for datareposrc properties.
 */
static void
gst_data_repo_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDataRepoSrc *src;

  g_return_if_fail (GST_IS_DATA_REPO_SRC (object));

  src = GST_DATA_REPO_SRC (object);

  switch (prop_id) {
    case PROP_LOCATION:
      gst_data_repo_src_set_location (src, g_value_get_string (value), NULL);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 * @brief Getter datareposrc properties
 */
static void
gst_data_repo_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDataRepoSrc *src;

  g_return_if_fail (GST_IS_DATA_REPO_SRC (object));

  src = GST_DATA_REPO_SRC (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_value_set_string (value, src->filename);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 * @brief Function to read tensors
 */
static GstFlowReturn
gst_data_repo_src_read_tensors (GstDataRepoSrc * src, GstBuffer ** buffer)
{
  int i = 0;
  GstBuffer *buf;
  guint to_read, byte_read;
  int ret;
  guint8 *data;
  GstMemory *mem[MAX_ITEM] = { 0, };
  GstMapInfo info[MAX_ITEM];

  buf = gst_buffer_new ();

  /** @todo : features and labels indexing with featuer and label index property */
  for (i = 0; i < 2; i++) {
    mem[i] = gst_allocator_alloc (NULL, src->tensors_size[i], NULL);

    if (!gst_memory_map (mem[i], &info[i], GST_MAP_WRITE)) {
      GST_ERROR_OBJECT (src, "Could not map GstMemory[%d]", i);
      goto error;
    }

    data = info[i].data;

    byte_read = 0;
    to_read = src->tensors_size[i];
    while (to_read > 0) {
      GST_LOG_OBJECT (src,
          "Reading %d bytes at offset 0x%" G_GINT64_MODIFIER "x", to_read,
          src->offset + byte_read);
      errno = 0;
      ret = read (src->fd, data + byte_read, to_read);
      GST_LOG_OBJECT (src, "Read: %d", ret);
      if (ret < 0) {
        if (errno == EAGAIN || errno == EINTR)
          continue;
        goto could_not_read;
      }
      /* files should eos if they read 0 and more was requested */
      if (ret == 0) {
        /* .. but first we should return any remaining data */
        if (byte_read > 0)
          break;
        goto eos;
      }
      to_read -= ret;
      byte_read += ret;

      src->read_position += ret;
      src->offset += ret;
    }

    if (mem[i])
      gst_memory_unmap (mem[i], &info[i]);

    gst_buffer_append_memory (buf, mem[i]);
  }

  *buffer = buf;

  return GST_FLOW_OK;

could_not_read:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, READ, (NULL), GST_ERROR_SYSTEM);
    gst_memory_unmap (mem[0], &info[0]);
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
eos:
  {
    GST_DEBUG ("EOS");
    gst_memory_unmap (mem[0], &info[0]);
    gst_buffer_unref (buf);
    return GST_FLOW_EOS;
  }
error:
  gst_buffer_unref (buf);
  return GST_FLOW_ERROR;
}


/**
 * @brief Get image filename
 */
static gchar *
gst_data_repo_src_get_image_filename (GstDataRepoSrc * src)
{
  gchar *filename = NULL;

  g_return_val_if_fail (src->media_type == _NNS_IMAGE, NULL);

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  /* let's set value by property */
  filename = g_strdup_printf (src->filename, src->frame_index);
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

  return filename;
}

/**
 * @brief Function to read multi image files
 */
static GstFlowReturn
gst_data_repo_src_read_multi_images (GstDataRepoSrc * src, GstBuffer ** buffer)
{
  gsize size;
  gchar *data;
  gchar *filename;
  GstBuffer *buf;
  gboolean ret;
  GError *error = NULL;

  filename = gst_data_repo_src_get_image_filename (src);
  GST_DEBUG_OBJECT (src, "Reading from file \"%s\".", filename);

  /* Try to read one image */
  ret = g_file_get_contents (filename, &data, &size, &error);
  if (!ret) {
    if (src->successful_read) {
      /* If we've read at least one buffer successfully, not finding the next file is EOS. */
      g_free (filename);
      if (error != NULL)
        g_error_free (error);
      return GST_FLOW_EOS;
    }
    goto handle_error;
  }

  /* Success reading on image */
  src->successful_read = TRUE;
  GST_DEBUG_OBJECT (src, "file size is %zd", size);
  /* let's set value by property */
  src->frame_index++;

  buf = gst_buffer_new ();
  gst_buffer_append_memory (buf,
      gst_memory_new_wrapped (0, data, size, 0, size, data, g_free));
  GST_DEBUG_OBJECT (src, "read file \"%s\".", filename);

  g_free (filename);
  *buffer = buf;

  return GST_FLOW_OK;

handle_error:
  {
    if (error != NULL) {
      GST_ELEMENT_ERROR (src, RESOURCE, READ,
          ("Error while reading from file \"%s\".", filename),
          ("%s", error->message));
      g_error_free (error);
    } else {
      GST_ELEMENT_ERROR (src, RESOURCE, READ,
          ("Error while reading from file \"%s\".", filename),
          ("%s", g_strerror (errno)));
    }
    g_free (filename);
    return GST_FLOW_ERROR;
  }
}

/**
 * @brief Function to read others media type (video, audio, octet and text)
 */
static GstFlowReturn
gst_data_repo_src_read_others (GstDataRepoSrc * src, GstBuffer ** buffer)
{
  GstBuffer *buf;
  guint to_read, byte_read;
  int ret;
  guint8 *data;
  GstMemory *mem;
  GstMapInfo info;

  mem = gst_allocator_alloc (NULL, src->media_size, NULL);

  if (!gst_memory_map (mem, &info, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (src, "Could not map GstMemory");
    goto error;
  }

  data = info.data;

  byte_read = 0;
  to_read = src->media_size;
  while (to_read > 0) {
    GST_LOG_OBJECT (src, "Reading %d bytes at offset 0x%" G_GINT64_MODIFIER "x",
        to_read, src->offset + byte_read);
    errno = 0;
    ret = read (src->fd, data + byte_read, to_read);
    GST_LOG_OBJECT (src, "Read: %d", ret);
    if (ret < 0) {
      if (errno == EAGAIN || errno == EINTR)
        continue;
      goto could_not_read;
    }
    /* files should eos if they read 0 and more was requested */
    if (ret == 0) {
      /* .. but first we should return any remaining data */
      if (byte_read > 0)
        break;
      goto eos;
    }
    to_read -= ret;
    byte_read += ret;

    src->read_position += ret;
    src->offset += ret;
  }

  if (mem)
    gst_memory_unmap (mem, &info);

  buf = gst_buffer_new ();
  gst_buffer_append_memory (buf, mem);

  *buffer = buf;
  return GST_FLOW_OK;

could_not_read:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, READ, (NULL), GST_ERROR_SYSTEM);
    gst_memory_unmap (mem, &info);
    return GST_FLOW_ERROR;
  }
eos:
  {
    GST_DEBUG ("EOS");
    gst_memory_unmap (mem, &info);
    return GST_FLOW_EOS;
  }
error:
  return GST_FLOW_ERROR;
}

/**
 * @brief Function to create a buffer
 */
static GstFlowReturn
gst_data_repo_src_create (GstPushSrc * pushsrc, GstBuffer ** buffer)
{
  GstDataRepoSrc *src;
  src = GST_DATA_REPO_SRC (pushsrc);

  switch (src->media_type) {
    case _NNS_TENSOR:
      return gst_data_repo_src_read_tensors (src, buffer);
    case _NNS_IMAGE:
      return gst_data_repo_src_read_multi_images (src, buffer);
    case _NNS_VIDEO:
    case _NNS_AUDIO:
    case _NNS_TEXT:
    case _NNS_OCTET:
      return gst_data_repo_src_read_others (src, buffer);
    default:
      return GST_FLOW_ERROR;
  }
}

/**
 * @brief Start datareposrc, open the file
 */
static gboolean
gst_data_repo_src_start (GstBaseSrc * basesrc)
{
  struct_stat stat_results;
  gchar *filename = NULL;
  GstDataRepoSrc *src = GST_DATA_REPO_SRC (basesrc);
  int flags = O_RDONLY | O_BINARY;

  if (src->filename == NULL || src->filename[0] == '\0')
    goto no_filename;

  /* let's set value by property */
  src->frame_index = src->start_frame_index;

  if (src->media_type == _NNS_IMAGE) {
    filename = gst_data_repo_src_get_image_filename (src);
  } else {
    filename = g_strdup (src->filename);
  }

  GST_INFO_OBJECT (src, "opening file %s", filename);

  /* open the file */
  src->fd = g_open (filename, flags, 0);

  if (src->fd < 0)
    goto open_failed;

  /* check if it is a regular file, otherwise bail out */
  if (fstat (src->fd, &stat_results) < 0)
    goto no_stat;

  if (S_ISDIR (stat_results.st_mode))
    goto was_directory;

  if (S_ISSOCK (stat_results.st_mode))
    goto was_socket;

  /* record if it's a regular (hence seekable and lengthable) file */
  if (!S_ISREG (stat_results.st_mode))
    goto error_close;;

  src->read_position = 0;

  if (src->media_type == _NNS_IMAGE) {
    /* no longer used */
    g_close (src->fd, NULL);
    src->fd = 0;
  }

  g_free (filename);

  return TRUE;

  /* ERROR */
no_filename:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND,
        ("No file name specified for reading."), (NULL));
    goto error_exit;
  }
open_failed:
  {
    switch (errno) {
      case ENOENT:
        GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND, (NULL),
            ("No such file \"%s\"", src->filename));
        break;
      default:
        GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
            (("Could not open file \"%s\" for reading."), src->filename),
            GST_ERROR_SYSTEM);
        break;
    }
    goto error_exit;
  }
no_stat:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        (("Could not get info on \"%s\"."), src->filename), (NULL));
    goto error_close;
  }
was_directory:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        (("\"%s\" is a directory."), src->filename), (NULL));
    goto error_close;
  }
was_socket:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        (("File \"%s\" is a socket."), src->filename), (NULL));
    goto error_close;
  }

error_close:
  g_close (src->fd, NULL);
  src->fd = 0;
error_exit:
  g_free (filename);
  return FALSE;
}

/**
 * @brief Stop datareposrc, unmap and close the file
 */
static gboolean
gst_data_repo_src_stop (GstBaseSrc * basesrc)
{
  GstDataRepoSrc *src = GST_DATA_REPO_SRC (basesrc);

  /* close the file */
  g_close (src->fd, NULL);
  src->fd = 0;

  return TRUE;
}

/**
 * @brief Get caps for caps negotiation
 */
static GstCaps *
gst_data_repo_src_get_caps (GstBaseSrc * basesrc, GstCaps * filter)
{
  GstDataRepoSrc *src = GST_DATA_REPO_SRC (basesrc);
  GstCaps *caps = NULL;

  caps = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (src));
  caps = gst_caps_make_writable (caps);

  GST_DEBUG_OBJECT (src, "get caps: %" GST_PTR_FORMAT, caps);

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;
  }
  GST_DEBUG_OBJECT (src, "get caps: %" GST_PTR_FORMAT, caps);
  return caps;
}

/**
 * @brief Get tensors size
 */
static guint
gst_data_repo_src_get_tensors_size (GstDataRepoSrc * src, const GstCaps * caps)
{
  GstStructure *s;
  GstTensorsConfig config;
  guint size = 0;
  guint i = 0;

  g_return_val_if_fail (src != NULL, 0);
  g_return_val_if_fail (caps != NULL, 0);

  s = gst_caps_get_structure (caps, 0);
  if (!gst_tensors_config_from_structure (&config, s))
    return 0;

  for (i = 0; i < config.info.num_tensors; i++) {
    src->tensors_size[i] = gst_tensor_info_get_size (&config.info.info[i]);
    GST_DEBUG ("%dth size is %d", i, src->tensors_size[i]);
    size = size + src->tensors_size[i];
  }

  gst_tensors_config_free (&config);

  return size;
}

/**
 * @brief Get video size
 */
static guint
gst_data_repo_src_get_video_size (const GstCaps * caps)
{
  GstStructure *s;
  const gchar *format_str;
  gint width = 0, height = 0;
  GstVideoInfo video_info;
  guint size = 0;

  g_return_val_if_fail (caps != NULL, 0);

  s = gst_caps_get_structure (caps, 0);
  gst_video_info_init (&video_info);
  gst_video_info_from_caps (&video_info, caps);

  format_str = gst_structure_get_string (s, "format");
  width = GST_VIDEO_INFO_WIDTH (&video_info);
  height = GST_VIDEO_INFO_HEIGHT (&video_info);
  /** https://gstreamer.freedesktop.org/documentation/additional/design/mediatype-video-raw.html?gi-language=c */
  size = (guint) GST_VIDEO_INFO_SIZE (&video_info);
  GST_DEBUG ("format(%s), width(%d), height(%d): %d Byte/frame", format_str,
      width, height, size);

  return size;
}

/**
 * @brief Get audio size
 */
static guint
gst_data_repo_src_get_audio_size (const GstCaps * caps)
{
  GstStructure *s;
  const gchar *format_str;
  guint size = 0;
  gint rate = 0, channel = 0;
  GstAudioInfo audio_info;
  gint depth;

  g_return_val_if_fail (caps != NULL, 0);

  s = gst_caps_get_structure (caps, 0);
  gst_audio_info_init (&audio_info);
  gst_audio_info_from_caps (&audio_info, caps);

  format_str = gst_structure_get_string (s, "format");
  rate = GST_AUDIO_INFO_RATE (&audio_info);
  channel = GST_AUDIO_INFO_CHANNELS (&audio_info);
  depth = GST_AUDIO_INFO_DEPTH (&audio_info);

  size = channel * (depth / 8) * rate;
  GST_DEBUG ("format(%s), depth(%d), rate(%d), channel(%d): %d Bps", format_str,
      depth, rate, channel, size);

  return size;
}

/**
 * @brief Get media info from caps
 */
static gboolean
gst_data_repo_src_get_media_info (GstDataRepoSrc * src, const GstCaps * caps)
{
  GstStructure *s;
  guint size = 0;

  g_return_val_if_fail (src != NULL, FALSE);
  g_return_val_if_fail (caps != NULL, FALSE);

  s = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (s, "other/tensors")) {
    size = gst_data_repo_src_get_tensors_size (src, caps);
    src->media_type = _NNS_TENSOR;
  } else if (gst_structure_has_name (s, "video/x-raw")) {
    size = gst_data_repo_src_get_video_size (caps);
    src->media_type = _NNS_VIDEO;
  } else if (gst_structure_has_name (s, "audio/x-raw")) {
    size = gst_data_repo_src_get_audio_size (caps);
    src->media_type = _NNS_AUDIO;
  } else if (gst_structure_has_name (s, "text/x-raw")) {
    src->media_type = _NNS_TEXT;
  } else if (gst_structure_has_name (s, "application/octet-stream")) {
    src->media_type = _NNS_OCTET;
    size = 3176;                /* for test, let's get size from file */
  } else if (gst_structure_has_name (s, "image/png")
      || gst_structure_has_name (s, "image/jpeg")
      || gst_structure_has_name (s, "image/tiff")
      || gst_structure_has_name (s, "image/gif")) {
    src->media_type = _NNS_IMAGE;
    size = DEFAULT_BLOCKSIZE;
  } else {
    GST_ERROR_OBJECT (src, "Could not get a media type");
    return FALSE;
  }

  /** After caps negotiation, text, and octet only know the mimetype.
   *  need to get size from file. */
  if (!size) {
    GST_ERROR_OBJECT (src, "Could not get size");
    return FALSE;
  }
  src->media_size = size;

  GST_DEBUG_OBJECT (src, "Get media type is %d", src->media_type);

  return TRUE;
}

/**
 * @brief caps after caps negotiation
 */
static gboolean
gst_data_repo_src_set_caps (GstBaseSrc * basesrc, GstCaps * caps)
{
  int ret = FALSE;
  GstDataRepoSrc *src = GST_DATA_REPO_SRC (basesrc);

  GST_INFO_OBJECT (src, "set caps: %" GST_PTR_FORMAT, caps);

  ret = gst_data_repo_src_get_media_info (src, caps);

  return ret;
}
