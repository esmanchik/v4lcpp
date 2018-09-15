#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <iostream>
#include <fstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <vector>
#include <sstream>
#include <functional>
#include <bits/unique_ptr.h>

class v4ldev {
    int fd;

public:
    v4ldev(const char *path = "/dev/video0") {
        fd = open(path, O_RDWR /* required */ | O_NONBLOCK, 0);
    }

    ~v4ldev() {
        if (fd != -1) close(fd);
    }

    bool opened() { return fd != -1; }

    int xioctl(int request, void *arg) {
        int r;
        do r = ioctl (fd, request, arg);
        while (-1 == r && EINTR == errno);
        return r;
    }

    void *map(v4l2_buffer &buf) {
        return mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    }

    int sel() {
        int r;
        fd_set fds;
        struct timeval tv = {0};

        do {
            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);
        } while ((r == -1 && (errno == EINTR)));

//        FD_ZERO(&fds);
//        FD_SET(fd, &fds);
//        tv.tv_sec = 2;
//        r = select(fd+1, &fds, NULL, NULL, &tv);
        return r;
    }
};

using namespace std;

void throw_perror(function<void (stringstream & s)> f) {
    stringstream s;
    f(s);
    perror(s.str().c_str());
    throw runtime_error(s.str());
}

class capturer {
    static const int nbuffers = 30;

    unique_ptr<v4ldev> pdev;
    void *buffers[nbuffers];

public:
    void open(string from) {
        pdev = make_unique<v4ldev>(from.c_str());
        if (!pdev->opened()) {
            pdev.release();
            throw_perror([&from](auto &s) {
                s << "Failed to open " << from;
            });
        }
        v4ldev &dev = *pdev;
        struct v4l2_capability cap;
        if (dev.xioctl(VIDIOC_QUERYCAP, &cap) == -1) {
            throw_perror([&from](auto &s) {
                if (EINVAL == errno) {
                    s << from << " is no V4L2 device";
                } else {
                    s << "Failed to query capabilities of " << from;
                }
            });
        }

        if (cap.capabilities  & V4L2_CAP_VIDEO_CAPTURE) {
            //std::cout << "Can capture video" << std::endl;
        }

        struct v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 640;
        fmt.fmt.pix.height = 480;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // V4L2_PIX_FMT_MJPEG; // V4L2_PIX_FMT_SGRBG10;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (dev.xioctl(VIDIOC_S_FMT, &fmt) == -1)
        {
            throw_perror([&from](auto &s) {
                s << "Failed to set pixel format for " << from;
            });
        }

        struct v4l2_requestbuffers req = {0};
        req.count = nbuffers;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (dev.xioctl(VIDIOC_REQBUFS, &req) == -1)
        {
            throw_perror([&from](auto &s) {
                s << "Failed to request buffer for " << from;
            });
        }
        //std::cout << "buffers requested " << req.count << std::endl;

        if (req.count > nbuffers) {
            throw_perror([&req](auto &s) {
                s << "Got " << req.count << " buffers instead of 4";
            });
        }

        for (int i = 0; i < req.count; i++) {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (dev.xioctl(VIDIOC_QBUF, &buf) == -1)
            {
                throw_perror([i](auto &s) {
                    s << "Failed to request buffer " << i;
                });
            }
            buffers[i] = dev.map(buf);
        }

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(dev.xioctl(VIDIOC_STREAMON, &type) == -1)
        {
            throw_perror([&from](auto &s) {
                s << "Failed to start capturing from " << from;
            });
        }
    }

    #define CLIP(X) ( (X) > 255 ? 255 : (X) < 0 ? 0 : X)

    // YUV -> RGB
    #define C(Y) ( (Y) - 16  )
    #define D(U) ( (U) - 128 )
    #define E(V) ( (V) - 128 )

    #define YUV2R(Y, U, V) CLIP(( 298 * C(Y)              + 409 * E(V) + 128) >> 8)
    #define YUV2G(Y, U, V) CLIP(( 298 * C(Y) - 100 * D(U) - 208 * E(V) + 128) >> 8)
    #define YUV2B(Y, U, V) CLIP(( 298 * C(Y) + 516 * D(U)              + 128) >> 8)

    void decode_rgb(unsigned char *yuv, int n, vector<char> &rgb) {
        rgb.resize(n * 6 / 4);
        for (int i = 0, j = 0; i < n; i+=4) {
            int u  = yuv[i + 1];
            int y1 = yuv[i];
            int v  = yuv[i + 3];
            int y2 = yuv[i + 2];
            int r = YUV2R(y1, u, v);
            int g = YUV2G(y1, u, v);
            int b = YUV2B(y1, u, v);
            rgb[j] = r;
            rgb[j + 1] = g;
            rgb[j + 2] = b;
            r = YUV2R(y2, u, v);
            g = YUV2G(y2, u, v);
            b = YUV2B(y2, u, v);
            j += 3;
            rgb[j] = r;
            rgb[j + 1] = g;
            rgb[j + 2] = b;
            j += 3;
        }
    }

    void decode_mono(char *yuv, int n, vector<char> &frame) {
        frame.resize(n / 2);
        for (int i = 0; i < frame.size(); i++) {
            frame[i] = yuv[i * 2];
        }
    }

    void grab(vector<char> &frame) {
        if (!pdev) {
            throw_perror([](auto &s) {
                s << "Device is not opened";
            });
        }
        v4ldev &dev = *pdev;
        int r = dev.sel();
        if(-1 == r)
        {
            throw_perror([](auto &s) {
                s << "Waiting frame failed";
            });
        }

        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        r = dev.xioctl(VIDIOC_DQBUF, &buf);
        if(-1 == r)
        {
            throw_perror([](auto &s) {
                s << "Failed to retrieve frame";
            });
        }

        std::cout << "buffer " << buf.index << ", bytes " << buf.bytesused << std::endl;

        auto *p = (unsigned char *)buffers[buf.index];
        //decode_mono(p, buf.bytesused, frame);
        decode_rgb(p, buf.bytesused, frame);

        r = dev.xioctl(VIDIOC_QBUF, &buf);
        if(-1 == r)
        {
            throw_perror([](auto &s) {
                s << "Failed to free frame buffer";
            });
        }
    }
};

void capture(string from, int frames, int start, int each, string to) {
    std::vector<char> decoded(640 * 480);
    capturer cap;
    cap.open(from);
    for (int i = 0; i < frames; i++) {
        cap.grab(decoded);
        ofstream f;
        stringstream name(to);
        name << to << start + i % each << ".ppm";
        cout << "frame " << name.str() << ", bytes " << decoded.size() << std::endl;
        f.open(name.str(), std::ios::binary | std::ios::trunc);
        //f << "P5 640 480 255 ";
        f << "P6\n640 480\n255\n";
        f.write(decoded.data(), decoded.size());
        f.close();
    }
}

int main(int argc, char *argv[]) {
    string path = argc > 1 ? argv[1] : "/dev/video0";
    int frames = argc > 2 ? atoi(argv[2]) : 25 * 60;
    int start = argc > 3 ? atoi(argv[3]) : 100;
    int each = argc > 4 ? atoi(argv[4]) : 40;
    string pref = argc > 5 ? argv[5] : "frame";
    try {
        capture(path, frames, start, each, pref);
        return 0;
    } catch(runtime_error & e) {
        cerr << e.what() << endl;
        return -1;
    }
}