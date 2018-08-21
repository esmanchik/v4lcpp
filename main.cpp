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

void capture(string from, string to) {
    v4ldev dev(from.c_str());

    if (!dev.opened()) {
        throw_perror([&from](auto &s) {
            s << "Failed to open " << from;
        });
    }

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
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (dev.xioctl(VIDIOC_REQBUFS, &req) == -1)
    {
        throw_perror([&from](auto &s) {
            s << "Failed to request buffer for " << from;
        });
    }
    //std::cout << "buffers requested " << req.count << std::endl;

    if (req.count > 4) {
        throw_perror([&req](auto &s) {
            s << "Got " << req.count << " buffers instead of 4";
        });
    }

    void *buffers[4];
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

    int r = dev.sel();
    if(-1 == r)
    {
        throw_perror([](auto &s) {
            s << "Waiting frame failed";
        });
    }

    struct v4l2_buffer buf = {0};
    //for (int i = 0; i < 10; i++) {
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        r = dev.xioctl(VIDIOC_DQBUF, &buf);
    //    if (-1 != r) break;
    //}
    if(-1 == r)
    {
        throw_perror([](auto &s) {
            s << "Failed to retrieve frame";
        });
    }

    //std::cout << "buffer " << buf.index << ", bytes " << buf.bytesused << std::endl;

    char *p = (char *)buffers[buf.index];
    std::vector<char> decoded(buf.bytesused / 2);
    for (int i = 0; i < decoded.size(); i++) {
        decoded[i] = p[i * 2];
    }

    std::ofstream f;
    f.open(to, std::ios::binary | std::ios::trunc);
    f << "P5 640 480 255 ";
    f.write(decoded.data(), decoded.size());//buf.bytesused);
    f.close();
}

int main(int argc, char *argv[]) {
    string path = argc > 1 ? argv[1] : "/dev/video0";
    try {
        capture(path, "frame.pgm");
        return 0;
    } catch(runtime_error & e) {
        cerr << e.what() << endl;
        return -1;
    }
}