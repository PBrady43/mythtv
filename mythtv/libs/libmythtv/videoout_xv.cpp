/* Based on xqcam.c by Paul Chinn <loomer@svpal.org> */
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <math.h>
#include <time.h>

#include <map>
#include <iostream>
using namespace std;

#include "XJ.h"
#include "../libmyth/mythcontext.h"
#include "../libmyth/util.h"

extern "C" {
#include "../libavcodec/avcodec.h"
}

#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#define XMD_H 1
#include <X11/extensions/xf86vmode.h>

#include "yuv2rgb.h"

extern "C" {
extern int      XShmQueryExtension(Display*);
extern int      XShmGetEventBase(Display*);
extern XvImage  *XvShmCreateImage(Display*, XvPortID, int, char*, int, int, XShmSegmentInfo*);
}

struct XvData
{
    Window XJ_root;
    Window XJ_win;
    Window XJ_curwin;
    GC XJ_gc;
    Screen *XJ_screen;
    Display *XJ_disp;
    XShmSegmentInfo *XJ_SHMInfo;
    map<unsigned char *, XvImage *> buffers;
    map<unsigned char *, XImage *> xbuffers;
};

#define GUID_I420_PLANAR 0x30323449
#define GUID_YV12_PLANAR 0x32315659

int XJ_error_catcher(Display * d, XErrorEvent * xeev)
{
  d = d; 
  xeev = xeev;
  return 0;
}

XvVideoOutput::XvVideoOutput(void)
{
    XJ_started = 0; 
    xv_port = -1; 
    scratchspace = NULL; 

    data = new XvData();
}

XvVideoOutput::~XvVideoOutput()
{
    Exit();
    delete data;
}

int XvVideoOutput::GetRefreshRate(void)
{
    if (!XJ_started)
        return -1;

    XF86VidModeModeLine mode_line;
    int dot_clock;

    if (!XF86VidModeGetModeLine(data->XJ_disp, XJ_screen_num, &dot_clock,
                                &mode_line))
        return -1;

    double rate = (double)((double)(dot_clock * 1000.0) /
                           (double)(mode_line.htotal * mode_line.vtotal));

    rate = 1000000.0 / rate;

    return (int)rate;
}

bool XvVideoOutput::Init(int width, int height, float aspect, int num_buffers, 
                         unsigned char **out_buffers, unsigned int winid,
                         int winx, int winy, int winw, int winh, 
                         unsigned int embedid)
{
    pthread_mutex_init(&lock, NULL);

    int (*old_handler)(Display *, XErrorEvent *);
    int i, ret;
    XJ_caught_error = 0;
  
    unsigned int p_version, p_release, p_request_base, p_event_base, 
                 p_error_base;
    int p_num_adaptors;

    XvAdaptorInfo *ai;

    XJ_width = width;
    XJ_height = height;

    XJ_aspect = aspect;

    XInitThreads();

    data->XJ_disp = XOpenDisplay(NULL);
    if (!data->XJ_disp) 
    {
        printf("open display failed\n"); 
        return false;
    }
 
    data->XJ_screen = DefaultScreenOfDisplay(data->XJ_disp);
    XJ_screen_num = DefaultScreen(data->XJ_disp);

    QString HorizScanMode = gContext->GetSetting("HorizScanMode", "overscan");
    QString VertScanMode = gContext->GetSetting("VertScanMode", "overscan");

    img_hscanf = gContext->GetNumSetting("HorizScanPercentage", 5) / 100.0;
    img_vscanf = gContext->GetNumSetting("VertScanPercentage", 5) / 100.0;
   
    img_xoff = gContext->GetNumSetting("xScanDisplacement", 0);
    img_yoff = gContext->GetNumSetting("yScanDisplacement", 0);


    if (VertScanMode == "underscan") 
    {
        img_vscanf = 0 - img_vscanf;
    }
    if (HorizScanMode == "underscan") 
    {
        img_hscanf = 0 - img_hscanf;
    }

    printf("Over/underscanning. V: %f, H: %f, XOff: %d, YOff: %d\n", 
           img_vscanf, img_hscanf, img_xoff, img_yoff);
 
    XJ_fixedaspect = gContext->GetNumSetting("FixedAspectRatio", 0);

    XJ_white = XWhitePixel(data->XJ_disp, XJ_screen_num);
    XJ_black = XBlackPixel(data->XJ_disp, XJ_screen_num);
  
    XJ_fullscreen = 0;
  
    data->XJ_root = DefaultRootWindow(data->XJ_disp);

    ret = XvQueryExtension(data->XJ_disp, &p_version, &p_release, 
                           &p_request_base, &p_event_base, &p_error_base);
    if (ret != Success) 
    {
        printf("XvQueryExtension failed.\n");
    }

    ai = NULL;
    xv_port = -1;
    ret = XvQueryAdaptors(data->XJ_disp, data->XJ_root,
                          (unsigned int *)&p_num_adaptors, &ai);

    if (ret != Success) 
    {
        printf("XvQueryAdaptors failed.\n");
        ai = NULL;
    }
    else
        if ( ai )
        {
            for (i = 0; i < p_num_adaptors; i++) 
            {
                if ((ai[i].type & XvInputMask) &&
                    (ai[i].type & XvImageMask))
                {
                    xv_port = ai[i].base_id;
                    break;
                }
            }
 
            if (p_num_adaptors > 0)
                XvFreeAdaptorInfo(ai);
        }

    use_shm = 0;
    char *dispname = DisplayString(data->XJ_disp);

    if ((dispname) &&
        (*dispname == ':'))
        use_shm = XShmQueryExtension(data->XJ_disp);

    // can be used to force non-Xv mode as well as non-Xv/non-Shm mode
    if (getenv("NO_XV"))
    {
        xv_port = -1;
    }
    if (getenv("NO_SHM"))
    {
        xv_port = -1;
        use_shm = 0;
    }

#ifndef QWS
    GetMythTVGeometry(data->XJ_disp, XJ_screen_num,
                      &XJ_screenx, &XJ_screeny, 
                      &XJ_screenwidth, &XJ_screenheight);
#endif

    oldx = curx = winx; 
    oldy = cury = winy;
    oldw = curw = winw;
    oldh = curh = winh;

    dispx = 0; dispy = 0;
    dispw = curw; disph = curh;
    imgx = curx; imgy = cury;
    imgw = XJ_width; imgh = XJ_height;

    embedding = false;

    if (winid <= 0)
    {
        cerr << "Bad winid given to output\n";
        return false;
    }

    data->XJ_curwin = data->XJ_win = winid;
    dispx = 0;
    dispy = 0;
    
    if (embedid > 0)
        data->XJ_curwin = data->XJ_win = embedid;

    old_handler = XSetErrorHandler(XJ_error_catcher);
    XSync(data->XJ_disp, 0);

    if (xv_port == -1)
    {
        printf("***\n");
        printf("* Couldn't find Xv support, falling back to non-Xv mode.\n");
        printf("* MythTV performance will be much slower since color\n");
        printf("* conversion and scaling will be done in software.\n");
        printf("* Consider upgrading your video card or X server if\n");
        printf("* you would like better performance.\n");

        if (!use_shm)
        {
            printf("***\n" );
            printf("* No XShm support found, MythTV may be very slow and "
                    "consume lots of cpu.\n");
        }
    }

    int formats;
    XvImageFormatValues *fo;
    bool foundimageformat = false;

    if ( xv_port != -1 )
    {
        fo = XvListImageFormats(data->XJ_disp, xv_port, &formats);
        for (i = 0; i < formats; i++)
        {
            if (fo[i].id == GUID_I420_PLANAR)
            {
                foundimageformat = true;
                colorid = GUID_I420_PLANAR;
            }
        }

        if (!foundimageformat)
        {
            for (i = 0; i < formats; i++)
            {
                if (fo[i].id == GUID_YV12_PLANAR)
                {
                    foundimageformat = true;
                    colorid = GUID_YV12_PLANAR;
                }
            }
        } 

        if (!foundimageformat)
        {
            printf("Couldn't find the proper Xv image format\n");
            exit(0);
        }

        if (fo)
            XFree(fo);

        printf("Using XV port %d\n", xv_port);
        XvGrabPort(data->XJ_disp, xv_port, CurrentTime);
    }

    data->XJ_gc = XCreateGC(data->XJ_disp, data->XJ_win, 0, 0);
    XJ_depth = DefaultDepthOfScreen(data->XJ_screen);

    if (xv_port != -1 )
    {
        data->XJ_SHMInfo = new XShmSegmentInfo[num_buffers];
        for (int i = 0; i < num_buffers; i++)
        {
            XvImage *image = XvShmCreateImage(data->XJ_disp, xv_port,
                                              colorid, 0, XJ_width, XJ_height, 
                                              &(data->XJ_SHMInfo)[i]);
            (data->XJ_SHMInfo)[i].shmid = shmget(IPC_PRIVATE, image->data_size,
                                             IPC_CREAT|0777);
            if ((data->XJ_SHMInfo)[i].shmid < 0)
            {
                perror("shmget failed:");
                return false;
            }
 
            image->data = (data->XJ_SHMInfo)[i].shmaddr = 
                             (char *)shmat((data->XJ_SHMInfo)[i].shmid, 0, 0);
            // mark for delete immediately - it won't be removed until detach
            shmctl((data->XJ_SHMInfo)[i].shmid, IPC_RMID, 0);

            data->buffers[(unsigned char *)image->data] = image;
            out_buffers[i] = (unsigned char *)image->data;

            (data->XJ_SHMInfo)[i].readOnly = False;

            XShmAttach(data->XJ_disp, &(data->XJ_SHMInfo)[i]);
            XSync(data->XJ_disp, 0);
        }
    }
    else if (use_shm)
    {
        data->XJ_SHMInfo = new XShmSegmentInfo[num_buffers];
        for (int i = 0; i < num_buffers; i++)
        {
            XImage *image = XShmCreateImage(data->XJ_disp,
                                    DefaultVisual(data->XJ_disp, XJ_screen_num),
                                    XJ_depth, ZPixmap, 0,
                                    &(data->XJ_SHMInfo)[i],
                                    curw, curh);
            (data->XJ_SHMInfo)[i].shmid = shmget(IPC_PRIVATE,
                                    image->bytes_per_line * image->height,
                                    IPC_CREAT|0777);
            if ((data->XJ_SHMInfo)[i].shmid < 0)
            {
                perror("shmget failed:");
                return false;
            }
 
            image->data = (data->XJ_SHMInfo)[i].shmaddr = 
                             (char *)shmat((data->XJ_SHMInfo)[i].shmid, 0, 0);
            // mark for delete immediately - it won't be removed until detach
            shmctl((data->XJ_SHMInfo)[i].shmid, IPC_RMID, 0);

            data->xbuffers[(unsigned char *)image->data] = image;
            out_buffers[i] = (unsigned char *)image->data;

            (data->XJ_SHMInfo)[i].readOnly = False;

            if (!XShmAttach(data->XJ_disp, &(data->XJ_SHMInfo)[i]))
            {
                perror("XShmAttach() failed:");
                return false;
            }
            XSync(data->XJ_disp, 0);
        }
    }
    else
    {
        for (int i = 0; i < num_buffers; i++)
        {
            char *sbuf = new char[XJ_depth / 8 * XJ_screenwidth * XJ_screenheight];
            XImage *image = XCreateImage(data->XJ_disp,
                                        DefaultVisual(data->XJ_disp, 0),
                                        XJ_depth, ZPixmap, 0, sbuf,
                                        curw, curh,
                                        XJ_depth, 0);

            data->xbuffers[(unsigned char *)image->data] = image;
            out_buffers[i] = (unsigned char *)image->data;

            XSync(data->XJ_disp, 0);
        }
    }

    XSetErrorHandler(old_handler);

    if (XJ_caught_error) 
    {
        return false;
    }

    MoveResize();
  
    XJ_started = 1;

    if ((xv_port != -1) &&
        (colorid != GUID_I420_PLANAR))
    {
        scratchspace = new unsigned char[width * height * 3 / 2];
    }

    return true;
}

void XvVideoOutput::Exit(void)
{
    if (XJ_started) 
    {
        XJ_started = false;

        int i = 0;

        if ( xv_port != -1 )
        {
            map<unsigned char *, XvImage *>::iterator iter =
                data->buffers.begin();
            for(; iter != data->buffers.end(); ++iter, ++i) 
            {
                XShmDetach(data->XJ_disp, &(data->XJ_SHMInfo)[i]);
                if ((data->XJ_SHMInfo)[i].shmaddr)
                    shmdt((data->XJ_SHMInfo)[i].shmaddr);
                XFree(iter->second);
            }

            delete [] (data->XJ_SHMInfo);

            if (scratchspace)
                delete [] scratchspace;

            XvUngrabPort(data->XJ_disp, xv_port, CurrentTime);
        }
        else if (use_shm)
        {
            map<unsigned char *, XImage *>::iterator iter =
                data->xbuffers.begin();
            for(; iter != data->xbuffers.end(); ++iter, ++i) 
            {
                XShmDetach(data->XJ_disp, &(data->XJ_SHMInfo)[i]);
                if ((data->XJ_SHMInfo)[i].shmaddr)
                    shmdt((data->XJ_SHMInfo)[i].shmaddr);
                if ((data->XJ_SHMInfo)[i].shmid > 0)
                    shmctl((data->XJ_SHMInfo)[i].shmid, IPC_RMID, 0);
                XFree(iter->second);
            }

            delete [] (data->XJ_SHMInfo);
        }
        else
        {
            // XXX Check this later -- XDestroyImage instead?
            map<unsigned char *, XImage *>::iterator iter =
                data->xbuffers.begin();
            for(; iter != data->xbuffers.end(); ++iter, ++i) 
            {
                XFree(iter->second);
            }
        }

        XFreeGC(data->XJ_disp, data->XJ_gc);
        XCloseDisplay(data->XJ_disp);
    }
}

void XvVideoOutput::ToggleFullScreen(void)
{
    if ( xv_port == -1 )
        return;

    pthread_mutex_lock(&lock);

    if (XJ_fullscreen)
    {
        XJ_fullscreen = 0; 

        curx = oldx; cury = oldy; curw = oldw; curh = oldh;
    } 
    else 
    {
        XJ_fullscreen = 1;
        oldx = curx; oldy = cury; oldw = curw; oldh = curh;

        curx = XJ_screenx;
        cury = XJ_screeny;
        curw = XJ_screenwidth;
        curh = XJ_screenheight + 4;
    }

    dispx = 0;
    dispy = 0;
    dispw = curw;
    disph = curh;

    MoveResize();

    pthread_mutex_unlock(&lock);
}
  
void XvVideoOutput::EmbedInWidget(unsigned long wid, int x, int y, int w, int h)
{
    if (embedding)
        return;

    pthread_mutex_lock(&lock);
    data->XJ_curwin = wid;

    olddispx = dispx;
    olddispy = dispy;
    olddispw = dispw;
    olddisph = disph;

    dispxoff = dispx = x;
    dispyoff = dispy = y;
    dispwoff = dispw = w;
    disphoff = disph = h;

    embedding = true;

    MoveResize();

    pthread_mutex_unlock(&lock);
}
 
void XvVideoOutput::StopEmbedding(void)
{
    if (!embedding)
        return;

    pthread_mutex_lock(&lock);

    dispx = olddispx;
    dispy = olddispy;
    dispw = olddispw;
    disph = olddisph;

    data->XJ_curwin = data->XJ_win;

    embedding = false;

    MoveResize();

    pthread_mutex_unlock(&lock);
}

void XvVideoOutput::PrepareFrame(unsigned char *buffer, int width, int height)
{
    if (xv_port != -1)
    {
        XvImage *image = data->buffers[buffer];
        if (colorid == GUID_YV12_PLANAR)
        {
            memcpy(scratchspace, (unsigned char *)image->data + (width * height),
                   width * height / 4);
            memcpy((unsigned char *)image->data + (width * height),
                   (unsigned char *)image->data + (width * height) * 5 / 4,
                   width * height / 4);
            memcpy((unsigned char *)image->data + (width * height) * 5 / 4,
                   scratchspace, width * height / 4);
        }

        pthread_mutex_lock(&lock);

        XvShmPutImage(data->XJ_disp, xv_port, data->XJ_curwin, data->XJ_gc,
                      image, imgx, imgy, imgw, imgh, dispxoff, dispyoff,
                      dispwoff, disphoff, False);
        XSync(data->XJ_disp, False);

        pthread_mutex_unlock(&lock);
    }
    else
    {
        static long long framesShown = 0;
        static int showFrame = 1;
        static int fps = 0;
        static time_t stop_time;

        // bad way to throttle frame display for non-Xv mode.
        // calculate fps we can do and skip enough frames so we don't exceed.
        if (framesShown == 0)
            stop_time = time(NULL) + 4;

        if ((!fps) &&
            (time(NULL) > stop_time))
        {
            fps = (int)(framesShown / 4);

            if (fps < 25)
            {
                showFrame = 120 / framesShown + 1;
                printf("***\n" );
                printf( "* Your system is not capable of displaying the\n" );
                printf( "* full framerate at %dx%d resolution.  Frames\n",
                    curw, curh );
                printf( "* will be skipped in order to keep the audio and\n" );
                printf( "* video in sync.\n" );
            }
        }

        framesShown++;

        if ((showFrame != 1) &&
            (framesShown % showFrame))
            return;

        unsigned char *sbuf = new unsigned char[curw * curh * 4];
        XImage *image = data->xbuffers[buffer];
        AVPicture image_in, image_out;
        ImgReSampleContext *scontext;
        int av_format;

        avpicture_fill(&image_out, (uint8_t *)sbuf, PIX_FMT_YUV420P,
            curw, curh );

        if (( curw == width ) &&
            ( curh == height ))
        {
            memcpy( sbuf, image->data, width * height * 3 / 2 );
        }
        else
        {
            avpicture_fill(&image_in, (uint8_t *)image->data, PIX_FMT_YUV420P,
                width, height );
            scontext = img_resample_init(curw, curh, width, height);
            img_resample( scontext, &image_out, &image_in );
        }

        switch (image->bits_per_pixel)
        {
            case 16: av_format = PIX_FMT_RGB565; break;
            case 24: av_format = PIX_FMT_RGB24;  break;
            case 32: av_format = PIX_FMT_RGBA32; break;
            default: cerr << "Non Xv mode only supports 16, 24, and 32 bpp "
                         "displays\n";
                     exit(0);
        }

        avpicture_fill(&image_in, (uint8_t *)image->data, av_format,
            curw, curh );

        img_convert(&image_in, av_format, &image_out, PIX_FMT_YUV420P,
            curw, curh);

        pthread_mutex_lock(&lock);

        if (use_shm)
            XShmPutImage(data->XJ_disp, data->XJ_curwin, data->XJ_gc, image,
                      0, 0, 0, 0, curw, curh, False );
        else
            XPutImage(data->XJ_disp, data->XJ_curwin, data->XJ_gc, image, 
                      0, 0, 0, 0, curw, curh );

        pthread_mutex_unlock(&lock);

        delete [] sbuf;
    }
}

void XvVideoOutput::Show()
{
        XSync(data->XJ_disp, False);
}

int XvVideoOutput::CheckEvents(void)
{
    if (!XJ_started)
        return 0;

    XEvent Event;
    char buf[100];
    KeySym keySym;
    static XComposeStatus stat;
    int key = 0;
 
    while (XPending(data->XJ_disp))
    {
        XNextEvent(data->XJ_disp, &Event);

        switch (Event.type)
        {
            case KeyPress: 
            {
                XLookupString(&Event.xkey, buf, sizeof(buf), &keySym, &stat);
                key = ((keySym&0xff00) != 0?((keySym&0x00ff) + 256):(keySym));
                return key;
            } break;
            case ConfigureNotify: 
            {
                ResizeVideo(Event.xconfigure.x, Event.xconfigure.y,
                            Event.xconfigure.width, Event.xconfigure.height);
                return key;
            } break;
            default: break;
        }
    }

    return key;
}

void XvVideoOutput::ResizeVideo(int x, int y, int w, int h)
{
    if (XEventsQueued(data->XJ_disp, QueuedAlready))
        return;

    if (XJ_fullscreen || h >= XJ_screenheight && w >= XJ_screenwidth)
        return;

    if (oldx == x && oldy == y && oldw == w && oldh == h)
        return;

    if (oldx == (x+1) && oldy == (y+1) && oldw == w && oldh == h)
        return;

    if (XJ_fixedaspect)
    {
        if (w * 3 / 4 > h)
        {
            w = (int) (h * 4 / 3);
        }
        else
        {
            h = (int) (w * 3 / 4);
        }
    }

    oldx = curx = x;
    oldy = cury = y;
    oldw = curw = w;
    oldh = curh = h;

    dispx = 0;
    dispy = 0;
    dispw = curw;
    disph = curh;
    
    MoveResize();
    return;
}

void XvVideoOutput::GetDrawSize(int &xoff, int &yoff, int &width, int &height)
{
    xoff = imgx;
    yoff = imgy;
    width = imgw;
    height = imgh;
}

void XvVideoOutput::MoveResize(void)
{
    int yoff, xoff;

    XMoveResizeWindow(data->XJ_disp, data->XJ_win, curx, cury, curw, curh);
    XMapRaised(data->XJ_disp, data->XJ_win);

    XRaiseWindow(data->XJ_disp, data->XJ_win);
    XFlush(data->XJ_disp);

    // Preset all image placement and sizing variables.
    imgx = 0; imgy = 0;
    imgw = XJ_width; imgh = XJ_height;
    xoff = img_xoff; yoff = img_yoff;
    dispxoff = dispx; dispyoff = dispy;
    dispwoff = dispw; disphoff = disph;

/*
    Here we apply playback over/underscanning and offsetting (if any apply).

    It doesn't make any sense to me to offset an image such that it is clipped.
    Therefore, we only apply offsets if there is an underscan or overscan which
    creates "room" to move the image around. That is, if we overscan, we can 
    move the "viewport". If we underscan, we change where we place the image 
    into the display window. If no over/underscanning is performed, you just 
    get the full original image scaled into the full display area.
*/

    if (img_vscanf > 0) {
        // Veritcal overscan. Move the Y start point in original image.
        imgy = (int)ceil(XJ_height * img_vscanf);
        imgh = (int)ceil(XJ_height * (1 - 2 * img_vscanf));

        // If there is an offset, apply it now that we have a room.
        // To move the image down, move the start point up.
        if(yoff > 0) {
            // Can't offset the image more than we have overscanned.
            if(yoff > imgy) yoff = imgy;
            imgy -= yoff;
        }
        // To move the image up, move the start point down.
        if(yoff < 0) {
            // Again, can't offset more than overscanned.
            if( abs(yoff) > imgy ) yoff = 0 - imgy;
            imgy -= yoff;
        }
    }

    if (img_hscanf > 0) {
        // Horizontal overscan. Move the X start point in original image.
        imgx = (int)ceil(XJ_width * img_hscanf);
        imgw = (int)ceil(XJ_width * (1 - 2 * img_hscanf));
        if(xoff > 0) {
            if(xoff > imgx) xoff = imgx;
            imgx -= xoff;
        }
        if(xoff < 0) {
            if( abs(xoff) > imgx ) xoff = 0 - imgx;
            imgx -= xoff;
        }
    }

    float vscanf, hscanf;
    if (img_vscanf < 0) {
        // Veritcal underscan. Move the starting Y point in the display window.
        // Use the abolute value of scan factor.
        vscanf = fabs(img_vscanf);
        dispyoff = (int)ceil(disph * vscanf);
        disphoff = (int)ceil(disph * (1 - 2 * vscanf));
        // Now offset the image within the extra blank space created by 
        // underscanning.
        // To move the image down, increase the Y offset inside the display 
        // window.
        if(yoff > 0) {
            // Can't offset more than we have underscanned.
            if(yoff > dispyoff) yoff = dispyoff;
            dispyoff += yoff;
        }
        if(yoff < 0) {
            if( abs(yoff) > dispyoff ) yoff = 0 - dispyoff;
            dispyoff += yoff;
        }
    }

    if (img_hscanf < 0) {
        hscanf = fabs(img_hscanf);
        dispxoff = (int)ceil(dispw * hscanf);
        dispwoff = (int)ceil(dispw * (1 - 2 * hscanf));
        if(xoff > 0) {
            if(xoff > dispxoff) xoff = dispxoff;
            dispxoff += xoff;
        }
        if(xoff < 0) {
            if( abs(xoff) > dispxoff ) xoff = 0 - dispxoff;
            dispxoff += xoff;
        }
    }

    if (XJ_aspect >= 1.34)
    {
        int oldheight = disphoff;
        disphoff = (int)(dispwoff / XJ_aspect);
        dispyoff = (oldheight - disphoff) / 2;
    }
}
