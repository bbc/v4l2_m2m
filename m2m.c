/*
 *  V4L2 M2M video example
 *
 *  This program can be used and distributed without restrictions.
 *
 * This program is based on the examples provided with the V4L2 API
 * see https://linuxtv.org/docs.php for more information
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>         /* getopt_long() */

#include <fcntl.h>          /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define FMT_NUM_PLANES 1

enum io_method {
    IO_METHOD_READ,
    IO_METHOD_MMAP,
    IO_METHOD_USERPTR,
};

struct buffer {
    void   *start;
    size_t  length;
};

struct buffer_mp {
    void   *start[FMT_NUM_PLANES];
    size_t  length[FMT_NUM_PLANES];
};

static char            *dev_name;
static enum io_method   io = IO_METHOD_MMAP;
static int              fd = -1;
struct buffer          *buffers;
struct buffer          *buffers_out;
struct buffer_mp       *buffers_mp;
struct buffer_mp       *buffers_mp_out;
static unsigned int     n_buffers;
static unsigned int     n_buffers_out;
static int              m2m_enabled;
static int              multi_planar;
static char            *out_filename;
static FILE            *out_fp;
static int              force_format;
static int              frame_count = 70;
static char            *in_filename;
static FILE            *in_fp;

static void errno_exit(const char *s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
    int r;

    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static int stream_type(int stream_type) 
{
    if (multi_planar) {
        if (stream_type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
            return  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        else if (stream_type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
            return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    }

    return stream_type;
}

static void process_image(const void *ptr, int size)
{
    if (!out_fp)
        out_fp = fopen(out_filename, "wb");
    if (out_fp)
        fwrite(ptr, size, 1, out_fp);

    fflush(stderr);
    fprintf(stderr, ".");
}

static void process_image_mp(void *ptr[], unsigned int size[])
{
    unsigned int p;

    for (p = 0; p < FMT_NUM_PLANES; ++p)
        process_image(ptr[p], size[p]);
}

static void supply_input(void *buf, unsigned int buf_len, unsigned int *bytesused)
{
    unsigned char *buf_char = (unsigned char*)buf;
    if (in_fp) {
        *bytesused = fread(buf, 1, buf_len, in_fp);
        if (*bytesused != buf_len)
            fprintf(stderr, "Short read %u instead of %u\n", *bytesused, buf_len);
        else
            fprintf(stderr, "Read %u bytes. First 4 bytes %02x %02x %02x %02x\n", *bytesused, 
                    buf_char[0], buf_char[1], buf_char[2], buf_char[3]);
    }
}

unsigned long f_offset = 0;

static void supply_input_by_au(void *buf, unsigned int buf_len, unsigned int *bytesused)
{
    unsigned char *buf_char = (unsigned char*)buf,
                  aud[5]    = "\000\000\000\001\t";
    size_t bytes_read, au_length;
    void *b_offset;

    fseek(in_fp, f_offset, SEEK_SET);
    bytes_read = fread(buf, 1, buf_len, in_fp);

    b_offset = memmem(buf + 1, bytes_read, 
                      aud, 5);

    au_length = b_offset - buf;
    f_offset += au_length;
    *bytesused = au_length;

    memset(b_offset, 0, buf_len - au_length);

    fprintf(stderr, "Used %u bytes. First 8 bytes %02x %02x %02x %02x %02x %02x %02x %02x\n", 
            *bytesused, 
            buf_char[0], buf_char[1], buf_char[2], buf_char[3],
            buf_char[4], buf_char[5], buf_char[6], buf_char[7]);
}

static void supply_input_mp(void *buf[], unsigned int buf_len[], unsigned int *bytesused)
{
    unsigned int p;
    unsigned int tot_bytes = 0;

    for (p = 0; p < FMT_NUM_PLANES; ++p) {
        unsigned int bytes;
        supply_input_by_au(buf[p], buf_len[p], &bytes);
        tot_bytes += bytes;
    }

    *bytesused = tot_bytes;
}

static int read_frame(enum v4l2_buf_type type, struct buffer *bufs, unsigned int n_buffers)
{
    struct v4l2_buffer buf;
    unsigned int i;

    switch (io) {
    case IO_METHOD_READ:
        if (-1 == read(fd, bufs[0].start, bufs[0].length)) {
            switch (errno) {
            case EAGAIN:
                return 0;

            case EIO:
                /* Could ignore EIO, see spec. */

                /* fall through */

            default:
                errno_exit("read");
            }
        }

        process_image(bufs[0].start, bufs[0].length);
        break;

    case IO_METHOD_MMAP:
        CLEAR(buf);

        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
            switch (errno) {
            case EAGAIN:
                return 0;

            case EIO:
                /* Could ignore EIO, see spec. */

                /* fall through */

            default:
                errno_exit("VIDIOC_DQBUF");
            }
        }

        assert(buf.index < n_buffers);

        if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
            process_image(bufs[buf.index].start, buf.bytesused);
        else
            supply_input(bufs[buf.index].start, bufs[buf.index].length, &buf.bytesused);

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
        break;

    case IO_METHOD_USERPTR:
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;

        if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
            switch (errno) {
            case EAGAIN:
                return 0;

            case EIO:
                /* Could ignore EIO, see spec. */

                /* fall through */

            default:
                errno_exit("VIDIOC_DQBUF");
            }
        }

        for (i = 0; i < n_buffers; ++i)
            if (buf.m.userptr == (unsigned long)bufs[i].start
                && buf.length == bufs[i].length)
                break;

        assert(i < n_buffers);

        process_image((void *)buf.m.userptr, buf.bytesused);

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
        break;
    }

    return 1;
}

static int read_frame_mmap_mp(enum v4l2_buf_type type, struct buffer_mp *bufs, unsigned int n_buffers)
{
    struct v4l2_buffer buf;
    struct v4l2_plane  planes[FMT_NUM_PLANES];

    CLEAR(buf);
    CLEAR(planes);

    buf.type     = type;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.length   = FMT_NUM_PLANES;
    buf.m.planes = planes;

    if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
        case EAGAIN:
            return 0;

        case EIO:
            /* Could ignore EIO, see spec. */

            /* fall through */

        default:
            errno_exit("VIDIOC_DQBUF");
        }
    }

    assert(buf.index < n_buffers);

    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
        process_image_mp(bufs[buf.index].start, bufs[buf.index].length);
    else
        supply_input_mp(bufs[buf.index].start, bufs[buf.index].length, &buf.bytesused);

    if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
        errno_exit("VIDIOC_QBUF");

    return 1;
}

static void stop_capture(enum v4l2_buf_type type)
{
    type = stream_type(type); // change type if multi-planar

    if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
        errno_exit("VIDIOC_STREAMOFF");
}

static void stop_capturing(void)
{
    switch (io) {
    case IO_METHOD_READ:
        /* Nothing to do. */
        break;

    case IO_METHOD_MMAP:
    case IO_METHOD_USERPTR:
        stop_capture(V4L2_BUF_TYPE_VIDEO_CAPTURE);

        if (m2m_enabled) 
            stop_capture(V4L2_BUF_TYPE_VIDEO_OUTPUT);
        break;
    }
}

static void start_capturing_mmap(enum v4l2_buf_type type, struct buffer *bufs, unsigned int n_bufs)
{
    unsigned int i;

    for (i = 0; i < n_bufs; ++i) {
        struct v4l2_buffer buf;

        CLEAR(buf);
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
            supply_input(bufs[i].start, bufs[i].length, &buf.bytesused);

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
    }
    
    if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
        errno_exit("VIDIOC_STREAMON");
}

static void start_capturing_mmap_mp(enum v4l2_buf_type type, struct buffer_mp *bufs, unsigned int n_bufs)
{
    unsigned int i;

    for (i = 0; i < n_bufs; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane  planes[FMT_NUM_PLANES];

        CLEAR(buf);
        CLEAR(planes);
        buf.type     = type;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.length   = FMT_NUM_PLANES;
        buf.m.planes = planes;

        if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
            supply_input_mp(bufs[i].start, bufs[i].length, &buf.bytesused);

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
    }
    
    if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
        errno_exit("VIDIOC_STREAMON");
}

static void start_capturing(void)
{
    unsigned int i;
    enum v4l2_buf_type type;

    switch (io) {
    case IO_METHOD_READ:
        /* Nothing to do. */
        break;

    case IO_METHOD_MMAP:
        if (multi_planar)
            start_capturing_mmap_mp(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, buffers_mp, n_buffers);
        else
            start_capturing_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE, buffers, n_buffers);

        if (m2m_enabled) {
            if (multi_planar)
                start_capturing_mmap_mp(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, buffers_mp_out, n_buffers_out);
            else
                start_capturing_mmap(V4L2_BUF_TYPE_VIDEO_OUTPUT, buffers, n_buffers_out);
        }
        break;

    case IO_METHOD_USERPTR:
        for (i = 0; i < n_buffers; ++i) {
            struct v4l2_buffer buf;

            CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_USERPTR;
            buf.index = i;
            buf.m.userptr = (unsigned long)buffers[i].start;
            buf.length = buffers[i].length;

            if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                errno_exit("VIDIOC_QBUF");
        }
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
            errno_exit("VIDIOC_STREAMON");
        break;
    }
}

static void unmap_buffers(struct buffer *buf, unsigned int n)
{
    unsigned int b;

    for (b = 0; b < n; ++b)
        if (-1 == munmap(buf[b].start, buf[b].length))
            errno_exit("munmap");
}

static void unmap_buffers_mp(struct buffer_mp *buf, unsigned int n)
{
    unsigned int b, p;

    for (b = 0; b < n; b++)
        for (p = 0; p < FMT_NUM_PLANES; p++)
            if (-1 == munmap(buf[b].start[p], buf[b].length[p]))
                errno_exit("munmap");
}

static void free_buffers_mmap(enum v4l2_buf_type type)
{
    struct v4l2_requestbuffers req;

    CLEAR(req);

    req.count = 0;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s does not support "
                    "memory mapping\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }
}

static void uninit_device(void)
{
    unsigned int i;

    switch (io) {   
    case IO_METHOD_READ:
        free(buffers[0].start);
        break;

    case IO_METHOD_MMAP:
        if (multi_planar) {
            unmap_buffers_mp(buffers_mp, n_buffers);
            free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
            if (m2m_enabled) {
                unmap_buffers_mp(buffers_mp_out, n_buffers_out);
                free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
            }
        } else {
            unmap_buffers(buffers, n_buffers);
            free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE);
            if (m2m_enabled) {
                unmap_buffers(buffers_out, n_buffers_out);
                free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_OUTPUT);
            }
        }
        break;

    case IO_METHOD_USERPTR:
        for (i = 0; i < n_buffers; ++i)
            free(buffers[i].start);
        break;
    }

    free(buffers);
}

static void init_read(unsigned int buffer_size)
{
    buffers = calloc(1, sizeof(*buffers));

    if (!buffers) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    buffers[0].length = buffer_size;
    buffers[0].start = malloc(buffer_size);

    if (!buffers[0].start) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }
}

static void init_mmap(enum v4l2_buf_type type, struct buffer **bufs_out, unsigned int *n_bufs)
{
    struct v4l2_requestbuffers req;
    struct buffer *bufs;
    unsigned int b;

    CLEAR(req);

    req.count  = 4;
    req.type   = type;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s does not support "
                    "memory mapping\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s\n",
                dev_name);
        exit(EXIT_FAILURE);
    }

    bufs = calloc(req.count, sizeof(*bufs));

    if (!bufs) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    for (b = 0; b < req.count; ++b) {
        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type   = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = b;

        if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
            errno_exit("VIDIOC_QUERYBUF");

        fprintf(stderr, "Mapping buffer %u, len %u\n", b, buf.length);
        bufs[b].length = buf.length;
        bufs[b].start =
            mmap(NULL /* start anywhere */,
                 buf.length,
                 PROT_READ | PROT_WRITE, /* required */
                 MAP_SHARED,             /* recommended */
                 fd, buf.m.offset);

        if (MAP_FAILED == bufs[b].start)
            errno_exit("mmap");
    }
    *n_bufs = b;
    *bufs_out = bufs;
}

static void init_mmap_mp(enum v4l2_buf_type type, struct buffer_mp **bufs_out, unsigned int *n_bufs)
{
    struct v4l2_requestbuffers req;
    struct buffer_mp *bufs;
    unsigned int b;

    CLEAR(req);

    req.count  = 4;
    req.type   = type;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s does not support "
                    "memory mapping\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s\n",
                dev_name);
        exit(EXIT_FAILURE);
    }

    bufs = calloc(req.count, sizeof(*bufs));

    if (!bufs) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    for (b = 0; b < req.count; ++b) {
        struct v4l2_buffer buf;
        struct v4l2_plane  planes[FMT_NUM_PLANES];
        unsigned int p;

        CLEAR(buf);

        buf.type   = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = b;
        /* length in struct v4l2_buffer in multi-planar API stores the size
         * of planes array. */
        buf.length   = FMT_NUM_PLANES;
        buf.m.planes = planes;

        if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
            errno_exit("VIDIOC_QUERYBUF");

        fprintf(stderr, "Mapping buffer %u:\n", b);
        for (p = 0; p < FMT_NUM_PLANES; ++p) {
            fprintf(stderr, "Mapping plane %u, len %u\n", p, 
                    buf.m.planes[p].length);

            bufs[b].length[p] = buf.m.planes[p].length;
            bufs[b].start[p] = 
                mmap(NULL, 
                     buf.m.planes[p].length,
                     PROT_READ | PROT_WRITE, /* required */
                     MAP_SHARED,             /* recommended */
                     fd, buf.m.planes[p].m.mem_offset);

            if (MAP_FAILED == bufs[b].start[p])
                errno_exit("mmap");
        }
    }
    *n_bufs = b;
    *bufs_out = bufs;
}

static void init_userp(unsigned int buffer_size)
{
    struct v4l2_requestbuffers req;

    CLEAR(req);

    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s does not support "
                 "user pointer i/o\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    buffers = calloc(4, sizeof(*buffers));

    if (!buffers) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
        buffers[n_buffers].length = buffer_size;
        buffers[n_buffers].start = malloc(buffer_size);

        if (!buffers[n_buffers].start) {
            fprintf(stderr, "Out of memory\n");
            exit(EXIT_FAILURE);
        }
    }
}

static void init_device_out(void)
{
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_format fmt;
    unsigned int min;

    CLEAR(cropcap);

    cropcap.type = stream_type(V4L2_BUF_TYPE_VIDEO_OUTPUT);

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
            switch (errno) {
            case EINVAL:
                /* Cropping not supported. */
                break;
            default:
                /* Errors ignored. */
                break;
            }
        }
    } else {
        /* Errors ignored. */
    }


    CLEAR(fmt);

    fmt.type = stream_type(V4L2_BUF_TYPE_VIDEO_OUTPUT);

    if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
        errno_exit("VIDIOC_G_FMT");

    if (force_format) {
        if (multi_planar) {
            fmt.fmt.pix_mp.width       = 1920;
            fmt.fmt.pix_mp.height      = 1080;
            fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
            fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
        } else {
            fmt.fmt.pix.width       = 640;
            fmt.fmt.pix.height      = 480;
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
            fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        }

        if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
            errno_exit("VIDIOC_S_FMT");

        /* Note VIDIOC_S_FMT may change width and height. */
    }

    /* Buggy driver paranoia. */
    if (multi_planar) {
        unsigned int p;

        for (p = 0; p < FMT_NUM_PLANES; ++p) {
            min = fmt.fmt.pix_mp.width * 2;
            if (fmt.fmt.pix_mp.plane_fmt[p].bytesperline > 0 &&
                fmt.fmt.pix_mp.plane_fmt[p].bytesperline < min)
                fmt.fmt.pix_mp.plane_fmt[p].bytesperline = min;
            min = fmt.fmt.pix_mp.plane_fmt[p].bytesperline * fmt.fmt.pix_mp.height;
            if (fmt.fmt.pix_mp.plane_fmt[p].sizeimage > 0 &&
                fmt.fmt.pix_mp.plane_fmt[p].sizeimage < min)
                fmt.fmt.pix_mp.plane_fmt[p].sizeimage = min;
        }
    } else {
        min = fmt.fmt.pix.width * 2;
        if (fmt.fmt.pix.bytesperline < min)
            fmt.fmt.pix.bytesperline = min;
        min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
        if (fmt.fmt.pix.sizeimage < min)
            fmt.fmt.pix.sizeimage = min;
    }

    switch (io) {
    case IO_METHOD_READ:
        init_read(fmt.fmt.pix.sizeimage);
        break;

    case IO_METHOD_MMAP:
        if (multi_planar)
            init_mmap_mp(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &buffers_mp_out, &n_buffers_out);
        else
            init_mmap(V4L2_BUF_TYPE_VIDEO_OUTPUT, &buffers_out, &n_buffers_out);
        break;

    case IO_METHOD_USERPTR:
        init_userp(fmt.fmt.pix.sizeimage);
        break;
    }

    struct v4l2_event_subscription sub;

    CLEAR(sub);

    sub.type = V4L2_EVENT_EOS;
    if (-1 == ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub))
        errno_exit("VIDIOC_SUBSCRIBE_EVENT");

    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (-1 == ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub))
        errno_exit("VIDIOC_SUBSCRIBE_EVENT");
}

static void init_device(void)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_format fmt;
    unsigned int min;

    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device\n",
                    dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_QUERYCAP");
        }
    }
/*
bcm2835-v4l2
8520 0005
V4L2_CAP_VIDEO_CAPTURE
V4L2_CAP_VIDEO_OVERLAY
V4L2_CAP_EXT_PIX_FORMAT
V4L2_CAP_READWRITE
V4L2_CAP_STREAMING
V4L2_CAP_DEVICE_CAPS

bcm2835-codec
8420 4000
V4L2_CAP_VIDEO_M2M_MPLANE
V4L2_CAP_EXT_PIX_FORMAT
V4L2_CAP_STREAMING
V4L2_CAP_DEVICE_CAPS
*/
    fprintf(stderr, "caps returned %04x\n", cap.capabilities);
    if (!(cap.capabilities & (V4L2_CAP_VIDEO_M2M|V4L2_CAP_VIDEO_M2M_MPLANE|V4L2_CAP_VIDEO_CAPTURE))) {
        fprintf(stderr, "%s is no video capture device\n",
                dev_name);
        exit(EXIT_FAILURE);
    }

    if (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)
        multi_planar = 1;

    switch (io) {
    case IO_METHOD_READ:
        if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
            fprintf(stderr, "%s does not support read i/o\n",
                    dev_name);
            exit(EXIT_FAILURE);
        }
        break;

    case IO_METHOD_MMAP:
    case IO_METHOD_USERPTR:
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            fprintf(stderr, "%s does not support streaming i/o\n",
                    dev_name);
            exit(EXIT_FAILURE);
        }
        break;
    }


    /* Select video input, video standard and tune here. */

    CLEAR(cropcap);

    cropcap.type = stream_type(V4L2_BUF_TYPE_VIDEO_CAPTURE);

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
            switch (errno) {
            case EINVAL:
                /* Cropping not supported. */
                break;
                
            default:
                /* Errors ignored. */
                break;
            }
        }
    } else {
        /* Errors ignored. */
    }


    CLEAR(fmt);

    fmt.type = stream_type(V4L2_BUF_TYPE_VIDEO_CAPTURE);

    if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
        errno_exit("VIDIOC_G_FMT");

    if (force_format) {
        if (multi_planar) {
            fmt.fmt.pix_mp.width       = 1920;
            fmt.fmt.pix_mp.height      = 1080;
            fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
            fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
        } else {
            fmt.fmt.pix.width       = 640;
            fmt.fmt.pix.height      = 480;
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
            fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        }

        if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
            errno_exit("VIDIOC_S_FMT");

        /* Note VIDIOC_S_FMT may change width and height. */
    }

    /* Buggy driver paranoia. */
    if (multi_planar) {
        unsigned int p;

        for (p = 0; p < FMT_NUM_PLANES; ++p) {
            min = fmt.fmt.pix_mp.width * 2;
            if (fmt.fmt.pix_mp.plane_fmt[p].bytesperline > 0 &&
                fmt.fmt.pix_mp.plane_fmt[p].bytesperline < min)
                fmt.fmt.pix_mp.plane_fmt[p].bytesperline = min;
            min = fmt.fmt.pix_mp.plane_fmt[p].bytesperline * fmt.fmt.pix_mp.height;
            if (fmt.fmt.pix_mp.plane_fmt[p].sizeimage > 0 &&
                fmt.fmt.pix_mp.plane_fmt[p].sizeimage < min)
                fmt.fmt.pix_mp.plane_fmt[p].sizeimage = min;
        }
    } else {
        min = fmt.fmt.pix.width * 2;
        if (fmt.fmt.pix.bytesperline < min)
            fmt.fmt.pix.bytesperline = min;
        min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
        if (fmt.fmt.pix.sizeimage < min)
            fmt.fmt.pix.sizeimage = min;
    }

    switch (io) {
    case IO_METHOD_READ:
        init_read(fmt.fmt.pix.sizeimage);
        break;

    case IO_METHOD_MMAP:
        if (multi_planar)
            init_mmap_mp(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &buffers_mp, &n_buffers);
        else
            init_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE, &buffers, &n_buffers);
        break;

    case IO_METHOD_USERPTR:
        init_userp(fmt.fmt.pix.sizeimage);
        break;
    }
    if (cap.capabilities & (V4L2_CAP_VIDEO_M2M|V4L2_CAP_VIDEO_M2M_MPLANE)) {
        init_device_out();
        m2m_enabled = 1;
        if (in_filename) {
            in_fp = fopen(in_filename, "rb");
            if (!in_fp)
                fprintf(stderr, "Failed to open input file %s\n", in_filename);
        }
    }
}

static void close_device(void)
{
    if (-1 == close(fd))
        errno_exit("close");

    fd = -1;
}

static void open_device(void)
{
    struct stat st;

    if (-1 == stat(dev_name, &st)) {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s is no devicen", dev_name);
        exit(EXIT_FAILURE);
    }

    fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

    if (-1 == fd) {
        fprintf(stderr, "Cannot open '%s': %d, %s\n",
                dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static void handle_event(void)
{
    struct v4l2_event ev;

    while (!ioctl(fd, VIDIOC_DQEVENT, &ev)) {
        switch (ev.type) {
        case V4L2_EVENT_SOURCE_CHANGE:
            fprintf(stderr, "Source changed\n");

            stop_capture(V4L2_BUF_TYPE_VIDEO_CAPTURE);

            if (multi_planar) {
                unmap_buffers_mp(buffers_mp, n_buffers);

                fprintf(stderr, "Unmapped all buffers\n");

                free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

                init_mmap_mp(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &buffers_mp, &n_buffers);

                start_capturing_mmap_mp(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, buffers_mp, n_buffers);
            } else {
                unmap_buffers(buffers, n_buffers);
                fprintf(stderr, "Unmapped all buffers\n");

                free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE);

                init_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE, &buffers, &n_buffers);

                start_capturing_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE, buffers, n_buffers);
            }
            break;
        case V4L2_EVENT_EOS:
            fprintf(stderr, "EOS\n");
            break;
        }
    }

}

static void mainloop(void)
{
    unsigned int count;

    count = frame_count;

    while (count-- > 0) {
        for (;;) {
            fd_set fds[3];
            fd_set *rd_fds = &fds[0]; /* for capture */
            fd_set *ex_fds = &fds[1]; /* for capture */
            fd_set *wr_fds = &fds[2]; /* for output */
            struct timeval tv;
            int r;

            if (rd_fds) {
                FD_ZERO(rd_fds);
                FD_SET(fd, rd_fds);
            }

            if (ex_fds) {
                FD_ZERO(ex_fds);
                FD_SET(fd, ex_fds);
            }

            if (wr_fds) {
                FD_ZERO(wr_fds);
                FD_SET(fd, wr_fds);
            }

            /* Timeout. */
            tv.tv_sec = 10;
            tv.tv_usec = 0;

            r = select(fd + 1, rd_fds, wr_fds, ex_fds, &tv);

            if (-1 == r) {
                if (EINTR == errno)
                    continue;
                errno_exit("select");
            }

            if (0 == r) {
                fprintf(stderr, "select timeout\n");
                exit(EXIT_FAILURE);
            }

            if (rd_fds && FD_ISSET(fd, rd_fds)) {
                fprintf(stderr, "Reading\n");
                if (multi_planar) {
                    if (read_frame_mmap_mp(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, buffers_mp, n_buffers))
                        break;
                } else {
                    if (read_frame(V4L2_BUF_TYPE_VIDEO_CAPTURE, buffers, n_buffers))
                        break;
                }
            }
            if (wr_fds && FD_ISSET(fd, wr_fds)) {
                fprintf(stderr, "Writing\n");
                if (multi_planar) {
                    if (read_frame_mmap_mp(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, buffers_mp_out, n_buffers_out))
                        break;
                } else {
                    if (read_frame(V4L2_BUF_TYPE_VIDEO_OUTPUT, buffers_out, n_buffers_out))
                        break;
                }
            }
            if (ex_fds && FD_ISSET(fd, ex_fds)) {
                fprintf(stderr, "Exception\n");
                handle_event();
            }
            /* EAGAIN - continue select loop. */
        }
    }
}

static void usage(FILE *fp, int argc, char **argv)
{
    fprintf(fp,
            "Usage: %s [options]\n\n"
            "Version 1.3\n"
            "Options:\n"
            "-d | --device name   Video device name [%s]\n"
            "-h | --help          Print this message\n"
            "-m | --mmap          Use memory mapped buffers [default]\n"
            "-r | --read          Use read() calls\n"
            "-u | --userp         Use application allocated buffers\n"
            "-o | --output name   Outputs stream to filename\n"
            "-f | --format        Force format to 640x480 YUYV\n"
            "-c | --count         Number of frames to grab [%i]\n"
            "-i | --infile name   Input filename for M2M devices\n"
            "",
            argv[0], dev_name, frame_count);
}

static const char short_options[] = "d:hmruo:fc:i:";

static const struct option
long_options[] = {
    { "device", required_argument, NULL, 'd' },
    { "help",   no_argument,       NULL, 'h' },
    { "mmap",   no_argument,       NULL, 'm' },
    { "read",   no_argument,       NULL, 'r' },
    { "userp",  no_argument,       NULL, 'u' },
    { "output", required_argument, NULL, 'o' },
    { "format", no_argument,       NULL, 'f' },
    { "count",  required_argument, NULL, 'c' },
    { "infile", required_argument, NULL, 'i' },
    { 0, 0, 0, 0 }
};

int main(int argc, char **argv)
{
    dev_name = "/dev/video0";

    for (;;) {
        int idx;
        int c;

        c = getopt_long(argc, argv,
                short_options, long_options, &idx);

        if (-1 == c)
            break;

        switch (c) {
        case 0: /* getopt_long() flag */
            break;

        case 'd':
            dev_name = optarg;
            break;

        case 'h':
            usage(stdout, argc, argv);
            exit(EXIT_SUCCESS);

        case 'm':
            io = IO_METHOD_MMAP;
            break;

        case 'r':
            io = IO_METHOD_READ;
            break;

        case 'u':
            io = IO_METHOD_USERPTR;
            break;

        case 'o':
            out_filename = optarg;
            break;

        case 'f':
            force_format++;
            break;

        case 'c':
            errno = 0;
            frame_count = strtol(optarg, NULL, 0);
            if (errno)
                errno_exit(optarg);
            break;

        case 'i':
            in_filename = optarg;
            break;

        default:
            usage(stderr, argc, argv);
            exit(EXIT_FAILURE);
        }
    }

    open_device();
    init_device();
    start_capturing();
    mainloop();
    stop_capturing();
    uninit_device();
    close_device();
    fprintf(stderr, "\n");
    return 0;
}
