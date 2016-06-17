/*
	 droid vnc server - Android VNC server
	 Copyright (C) 2009 Jose Pereira <onaips@gmail.com>

	 This library is free software; you can redistribute it and/or
	 modify it under the terms of the GNU Lesser General Public
	 License as published by the Free Software Foundation; either
	 version 3 of the License, or (at your option) any later version.

	 This library is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	 Lesser General Public License for more details.

	 You should have received a copy of the GNU Lesser General Public
	 License along with this library; if not, write to the Free Software
	 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
	 */

#ifdef __ANDROID__
# include <android/api-level.h>
#endif

#include "flinger.h"
#include "screenFormat.h"

#ifdef __ANDROID_API__

# if __ANDROID_API__ > 15
#  include <binder/IPCThreadState.h>
#  include <binder/ProcessState.h>
#  include <gui/SurfaceComposerClient.h>
#  include <gui/ISurfaceComposer.h>
#  include <ui/DisplayInfo.h>
#  include <ui/PixelFormat.h>

using namespace android;

static uint32_t DEFAULT_DISPLAY_ID = ISurfaceComposer::eDisplayIdMain;
// Maps orientations from DisplayInfo to ISurfaceComposer
static const uint32_t ORIENTATION_MAP[] = {
    ISurfaceComposer::eRotateNone, // 0 == DISPLAY_ORIENTATION_0
    ISurfaceComposer::eRotate270, // 1 == DISPLAY_ORIENTATION_90
    ISurfaceComposer::eRotate180, // 2 == DISPLAY_ORIENTATION_180
    ISurfaceComposer::eRotate90, // 3 == DISPLAY_ORIENTATION_270
};
static sp<IBinder> display;
uint32_t captureOrientation = ISurfaceComposer::eRotateNone;
# else
#  include <binder/IPCThreadState.h>
#  include <binder/ProcessState.h>
#  include <binder/IServiceManager.h>
#  include <binder/IMemory.h>
#  include <surfaceflinger/ISurfaceComposer.h>
#  include <surfaceflinger/SurfaceComposerClient.h>

using namespace android;

# endif //__ANDROID_API__ > 6
#endif //__ANDROID_API__

static ScreenshotClient * screenshotClient = NULL;

#if __ANDROID_API__ > 15

extern "C" screenFormat getscreenformat_flinger()
{
  screenFormat format;
  uint32_t f;

  format.width  = screenshotClient->getWidth();
  format.height = screenshotClient->getHeight();
  format.size = screenshotClient->getSize();
  f = screenshotClient->getFormat();
  format.bitsPerPixel = android::bitsPerPixel(f);
  if( f == android::PIXEL_FORMAT_RGB_565 )
  {
    format.redShift = 11;
    format.redMax = 16;
    format.greenShift = 5;
    format.greenMax = 10;
    format.blueShift = 0;
    format.blueMax = 4;
    format.alphaShift = 0;
    format.alphaMax = 0;
  }
  else
  {
    format.redShift = 24;
    format.redMax = 32;
    format.greenShift = 16;
    format.greenMax = 23;
    format.blueShift = 8;
    format.blueMax = 15;
    format.alphaShift = 0;
    format.alphaMax = 7;
    
  }
  return format;
}

extern "C" int init_flinger()
{
  L("--Initializing jellybean access method--\n");

  screenshotClient = new ScreenshotClient();

  display = SurfaceComposerClient::getBuiltInDisplay(DEFAULT_DISPLAY_ID);
  if (display == NULL) {
        L( "Unable to get handle for display\n");
        return -1;
  }
  
  Vector<DisplayInfo> configs;
  SurfaceComposerClient::getDisplayConfigs(display, &configs);
  int activeConfig = SurfaceComposerClient::getActiveConfig(display);
  if (static_cast<size_t>(activeConfig) >= configs.size()) {
      L("Active config not inside configs\n");
      return -1;
  }
  
  uint8_t displayOrientation = configs[activeConfig].orientation;
  uint32_t captureOrientation = ORIENTATION_MAP[displayOrientation];
  status_t result = screenshotClient->update(display, Rect(), 0, 0, 0, -1U, false, captureOrientation);
  if (!screenshotClient->getPixels())
    return -1;

  if (result != NO_ERROR)
    return -1;

  return 0;
}

extern "C" unsigned int *readfb_flinger()
{
  status_t result = screenshotClient->update(display, Rect(), 0, 0, 0, -1U, false, captureOrientation);
  return (unsigned int*)screenshotClient->getPixels();
}

#else

extern "C" screenFormat getscreenformat_flinger()
{
  //get format on PixelFormat struct
  PixelFormat f = screenshotClient->getFormat();

  PixelFormatInfo pf;
  getPixelFormatInfo(f,&pf);

  screenFormat format;
  format.bitsPerPixel = pf.bitsPerPixel;
  format.width = screenshotClient->getWidth();
  format.height =     screenshotClient->getHeight();
  format.size = pf.bitsPerPixel*format.width*format.height/CHAR_BIT;
  format.redShift = pf.l_red;
  format.redMax = pf.h_red;
  format.greenShift = pf.l_green;
  format.greenMax = pf.h_green-pf.h_red;
  format.blueShift = pf.l_blue;
  format.blueMax = pf.h_blue-pf.h_green;
  format.alphaShift = pf.l_alpha;
  format.alphaMax = pf.h_alpha-pf.h_blue;

  return format;
}

extern "C" int init_flinger()
{
  int errno;

  L("--Initializing gingerbread access method--\n");

  screenshotClient = new ScreenshotClient();
  errno = screenshotClient->update();
  if (!screenshotClient->getPixels())
    return -1;

  if (errno != NO_ERROR)
    return -1;

  return 0;
}

extern "C" unsigned int *readfb_flinger()
{
  screenshotClient->update();
  return (unsigned int*)screenshotClient->getPixels();
}
#endif //__ANDROID_API__ > 16

extern "C" void close_flinger()
{
  free(screenshotClient);
}

