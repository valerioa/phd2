/*
 *  cam_INDI.cpp
 *  PHD Guiding
 *
 *  Created by Geoffrey Hausheer.
 *  Copyright (c) 2009 Geoffrey Hausheer.
 *  All rights reserved.
 *
 *  Redraw for libindi/baseclient by Patrick Chevalley
 *  Copyright (c) 2014 Patrick Chevalley
 *  All rights reserved.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "phd.h"

#ifdef INDI_CAMERA

#include <iostream>
#include <fstream>

#include "config_INDI.h"
#include "camera.h"
#include "time.h"
#include "image_math.h"
#include "cam_INDI.h"

CameraINDI::CameraINDI()
{
    ClearStatus();
    // load the values from the current profile
    INDIhost = pConfig->Profile.GetString("/indi/INDIhost", _T("localhost"));
    INDIport = pConfig->Profile.GetLong("/indi/INDIport", 7624);
    INDICameraName = pConfig->Profile.GetString("/indi/INDIcam", _T("INDI Camera"));
    INDICameraCCD = pConfig->Profile.GetLong("/indi/INDIcam_ccd", 0);
    INDICameraPort = pConfig->Profile.GetString("/indi/INDIcam_port",_T(""));
    INDICameraForceVideo = pConfig->Profile.GetBoolean("/indi/INDIcam_forcevideo",false);
    Name = INDICameraName;
    SetCCDdevice();
    PropertyDialogType = PROPDLG_ANY;
    FullSize = wxSize(640,480);
    HasSubframes = true;
    m_bitsPerPixel = 0;
}

CameraINDI::~CameraINDI()
{
    disconnectServer();
}

void CameraINDI::ClearStatus()
{
    // reset properties pointer
    connection_prop = NULL;
    expose_prop = NULL;
    frame_prop = NULL;
    frame_type_prop = NULL;
    ccdinfo_prop = NULL;
    binning_prop = NULL;
    video_prop = NULL;
    camera_port = NULL;
    camera_device = NULL;
    pulseGuideNS_prop = NULL;
    pulseGuideEW_prop = NULL;
    // gui self destroy on lost connection
    gui = NULL;
    // reset connection status
    has_blob = false;
    Connected = false;
    ready = false;
    m_hasGuideOutput = false;
    PixSize = PixSizeX = PixSizeY = 0.0;
    cam_bp = NULL;
}

void CameraINDI::CheckState()
{
    // Check if the device has all the required properties for our usage.
    if (has_blob && camera_device && Connected && (expose_prop || video_prop)) {
        if (! ready) {
            //printf("Camera is ready\n");
            ready = true;
            first_frame = true;
            if (modal) {
                modal = false;
            }
        }
    }
}

void CameraINDI::newDevice(INDI::BaseDevice *dp)
{
    if (strcmp(dp->getDeviceName(), INDICameraName.mb_str(wxConvUTF8)) == 0) {
        // The camera object
        camera_device = dp;
    }
}

void CameraINDI::newSwitch(ISwitchVectorProperty *svp)
{
    // we go here every time a Switch state change
    //printf("Camera Receving Switch: %s = %i\n", svp->name, svp->sp->s);
    if (strcmp(svp->name, "CONNECTION") == 0) {
        ISwitch *connectswitch = IUFindSwitch(svp,"CONNECT");
        if (connectswitch->s == ISS_ON) {
            Connected = true;
        }
        else {
            if (ready) {
               ClearStatus();
               DisconnectWithAlert(_("INDI camera disconnected"), NO_RECONNECT);
            }
        }
    }
}

void CameraINDI::newMessage(INDI::BaseDevice *dp, int messageID)
{
    // we go here every time the camera driver send a message
    //printf("Camera Receving message: %s\n", dp->messageQueue(messageID));
}

void CameraINDI::newNumber(INumberVectorProperty *nvp)
{
    // we go here every time a Number value change
    //printf("Camera Receving Number: %s = %g\n", nvp->name, nvp->np->value);
    if (nvp == ccdinfo_prop) {
        PixSize = IUFindNumber(ccdinfo_prop,"CCD_PIXEL_SIZE")->value;
        PixSizeX = IUFindNumber(ccdinfo_prop,"CCD_PIXEL_SIZE_X")->value;
        PixSizeY = IUFindNumber(ccdinfo_prop,"CCD_PIXEL_SIZE_Y")->value;
        m_maxSize.x = IUFindNumber(ccdinfo_prop,"CCD_MAX_X")->value;
        m_maxSize.y = IUFindNumber(ccdinfo_prop,"CCD_MAX_Y")->value;
        FullSize = wxSize(m_maxSize.x / Binning, m_maxSize.y / Binning);
        m_bitsPerPixel = IUFindNumber(ccdinfo_prop, "CCD_BITSPERPIXEL")->value;
    }
    if (nvp == binning_prop) {
        MaxBinning = wxMin(binning_x->max, binning_y->max);
        Binning = wxMin(binning_x->value, binning_y->value);
        if (Binning > MaxBinning)
            Binning = MaxBinning;
        m_curBinning = Binning;
        FullSize = wxSize(m_maxSize.x / Binning, m_maxSize.y / Binning);
    }
}

void CameraINDI::newText(ITextVectorProperty *tvp)
{
    // we go here every time a Text value change
    //printf("Camera Receving Text: %s = %s\n", tvp->name, tvp->tp->text);
}

void  CameraINDI::newBLOB(IBLOB *bp)
{
    // we go here every time a new blob is available
    // this is normally the image from the camera
    //printf("Got camera blob %s \n",bp->name);
    if ((expose_prop)&&(!INDICameraForceVideo)) {
        if (bp->name == INDICameraBlobName) {
            cam_bp = bp;
            modal = false;
        }
    }
    else if (video_prop){
        cam_bp = bp;
        if (modal && (!stacking))
        {
            StackStream();
        }
    }
}

void CameraINDI::newProperty(INDI::Property *property)
{
    // Here we receive a list of all the properties after the connection
    // Updated values are not received here but in the newTYPE() functions above.
    // We keep the vector for each interesting property to send some data later.
    //const char* DeviName = property->getDeviceName();
    wxString PropName(property->getName());
    #ifdef INDI_PRE_1_1_0
      INDI_TYPE Proptype = property->getType();
    #else
      INDI_PROPERTY_TYPE Proptype = property->getType();
    #endif

    //printf("Camera Property: %s\n",property->getName());

    if (Proptype == INDI_BLOB) {
        //printf("Found BLOB property for %s %s\n", DeviName, PropName);
        if (PropName==INDICameraBlobName) {
           has_blob = 1;
           // set option to receive blob and messages for the selected CCD
           setBLOBMode(B_ALSO, INDICameraName.mb_str(wxConvUTF8), INDICameraBlobName.mb_str(wxConvUTF8));       
        }
    }
    else if (PropName == INDICameraCCDCmd + "EXPOSURE" && Proptype == INDI_NUMBER) {
        //printf("Found CCD_EXPOSURE for %s %s\n", DeviName, PropName);
        expose_prop = property->getNumber();
    }
    else if (PropName == INDICameraCCDCmd + "FRAME" && Proptype == INDI_NUMBER) {
        //printf("Found CCD_FRAME for %s %s\n", DeviName, PropName);
        frame_prop = property->getNumber();
        frame_x = IUFindNumber(frame_prop,"X");
        frame_y = IUFindNumber(frame_prop,"Y");
        frame_width = IUFindNumber(frame_prop,"WIDTH");
        frame_height = IUFindNumber(frame_prop,"HEIGHT");
    }
    else if (PropName == INDICameraCCDCmd + "FRAME_TYPE" && Proptype == INDI_SWITCH) {
        //printf("Found CCD_FRAME_TYPE for %s %s\n", DeviName, PropName);
        frame_type_prop = property->getSwitch();
    }
    else if (PropName == INDICameraCCDCmd + "BINNING" && Proptype == INDI_NUMBER) {
        //printf("Found CCD_BINNING for %s %s\n",DeviName, PropName);
        binning_prop = property->getNumber();
        binning_x = IUFindNumber(binning_prop,"HOR_BIN");
        binning_y = IUFindNumber(binning_prop,"VER_BIN");
        newNumber(binning_prop);
    }
    else if (((PropName == INDICameraCCDCmd + "VIDEO_STREAM")) && Proptype == INDI_SWITCH) {
        //printf("Found Video %s %s\n",DeviName, PropName);
        video_prop = property->getSwitch();
        has_old_videoprop = false;
    }
    else if (((PropName == "VIDEO_STREAM") ) && Proptype == INDI_SWITCH) {
        //printf("Found Video %s %s\n",DeviName, PropName);
        video_prop = property->getSwitch();
        has_old_videoprop = true;
    }
    else if (PropName == "DEVICE_PORT" && Proptype == INDI_TEXT) {
        //printf("Found device port for %s \n",DeviName);
        camera_port = property->getText();
    }
    else if (PropName == "CONNECTION" && Proptype == INDI_SWITCH) {
        //printf("Found CONNECTION for %s %s\n",DeviName, PropName);
        // Check the value here in case the device is already connected
        connection_prop = property->getSwitch();
        ISwitch *connectswitch = IUFindSwitch(connection_prop,"CONNECT");
        Connected = (connectswitch->s == ISS_ON);
    }
    else if (PropName == "DRIVER_INFO" && Proptype == INDI_TEXT) {
        if (camera_device && (camera_device->getDriverInterface() & INDI::BaseDevice::GUIDER_INTERFACE)) {
            m_hasGuideOutput = true; // Device supports guiding
        }
    }
    else if (PropName == "TELESCOPE_TIMED_GUIDE_NS" && Proptype == INDI_NUMBER){
        pulseGuideNS_prop = property->getNumber();
        pulseN_prop = IUFindNumber(pulseGuideNS_prop,"TIMED_GUIDE_N");
        pulseS_prop = IUFindNumber(pulseGuideNS_prop,"TIMED_GUIDE_S");
    }
    else if (PropName == "TELESCOPE_TIMED_GUIDE_WE" && Proptype == INDI_NUMBER){
        pulseGuideEW_prop = property->getNumber();
        pulseW_prop = IUFindNumber(pulseGuideEW_prop,"TIMED_GUIDE_W");
        pulseE_prop = IUFindNumber(pulseGuideEW_prop,"TIMED_GUIDE_E");
    }
    else if (PropName == INDICameraCCDCmd + "INFO" && Proptype == INDI_NUMBER) {
        ccdinfo_prop = property->getNumber();
        newNumber(ccdinfo_prop);
    }

    CheckState();
}

bool CameraINDI::Connect(const wxString& camId)
{
    // If not configured open the setup dialog
    if (INDICameraName == wxT("INDI Camera")) {
        CameraSetup();
    }
    Debug.Write(wxString::Format("INDI Camera connecting to device [%s]\n", INDICameraName));
    // define server to connect to.
    setServer(INDIhost.mb_str(wxConvUTF8), INDIport);
    // Receive messages only for our camera.
    watchDevice(INDICameraName.mb_str(wxConvUTF8));
    // Connect to server.
    if (connectServer()) {
       return !ready;
    }
    else {
       // last chance to fix the setup
       CameraSetup();
       setServer(INDIhost.mb_str(wxConvUTF8), INDIport);
       watchDevice(INDICameraName.mb_str(wxConvUTF8));
       if (connectServer()) {
          return !ready;
       }
       else {
          return true;
      }
    }
}

wxByte CameraINDI::BitsPerPixel()
{
    return m_bitsPerPixel;
}

bool CameraINDI::Disconnect()
{
    if (ready) {
       // Disconnect from server
       if (disconnectServer()){
          return false;
       }
       else return true;
    }
    else return true;
}

void CameraINDI::serverConnected()
{
    // After connection to the server
    modal = true;
    // wait for the device port property
    wxLongLong msec;
    msec = wxGetUTCTimeMillis();
    if (INDICameraPort.Length()) {  // the camera port is not mandatory
        while ((!camera_port) && wxGetUTCTimeMillis() - msec < 15 * 1000) {
            ::wxSafeYield();
        }
        // Set the port, this must be done before to try to connect the device
        if (camera_port) {
            char* porttext = (const_cast<char*>((const char*)INDICameraPort.mb_str()));
            camera_port->tp->text = porttext;
            sendNewText(camera_port);
        }
    }
    // Connect the camera device
    while ((!connection_prop) && wxGetUTCTimeMillis() - msec < 15 * 1000) {
         ::wxSafeYield();
    }
    connectDevice(INDICameraName.mb_str(wxConvUTF8));

    msec = wxGetUTCTimeMillis();
    while (modal && wxGetUTCTimeMillis() - msec < 30 * 1000) {
        ::wxSafeYield();
    }
    modal = false;
    if (ready)
    {
        Connected = true;
    }
    else {
        // In case we not get all the required properties or connection to the device failed
        pFrame->Alert(wxString::Format(_("Cannot connect to camera %s"), INDICameraName));
        Connected = false;
        Disconnect();
    }
}

void CameraINDI::serverDisconnected(int exit_code)
{
   // after disconnection we reset the connection status and the properties pointers
   ClearStatus();
   // in case the connection lost we must reset the client socket
   if (exit_code==-1) DisconnectWithAlert(_("INDI server disconnected"), NO_RECONNECT);
}

#ifndef INDI_PRE_1_0_0
void CameraINDI::removeDevice(INDI::BaseDevice *dp)
{
   ClearStatus();
   DisconnectWithAlert(_("INDI camera disconnected"), NO_RECONNECT);
}
#endif

void CameraINDI::ShowPropertyDialog()
{
    if (Connected) {
        // show the devices INDI dialog
        CameraDialog();
    }
    else {
        // show the server and device configuration
        CameraSetup();
    }
}

void CameraINDI::CameraDialog()
{
   if (gui) {
      gui->Show();
   }
   else {
      gui = new IndiGui();
      gui->child_window = true;
      gui->allow_connect_disconnect = false;
      gui->ConnectServer(INDIhost, INDIport);
      gui->Show();
   }
}

void CameraINDI::CameraSetup()
{
    // show the server and device configuration
    INDIConfig *indiDlg = new INDIConfig(wxGetActiveWindow(),TYPE_CAMERA);
    indiDlg->INDIhost = INDIhost;
    indiDlg->INDIport = INDIport;
    indiDlg->INDIDevName = INDICameraName;
    indiDlg->INDIDevCCD = INDICameraCCD;
    indiDlg->INDIDevPort = INDICameraPort;
    indiDlg->INDIForceVideo = INDICameraForceVideo;
    // initialize with actual values
    indiDlg->SetSettings();
    // try to connect to server
    indiDlg->Connect();
    if (indiDlg->ShowModal() == wxID_OK) {
        // if OK save the values to the current profile
        indiDlg->SaveSettings();
        INDIhost = indiDlg->INDIhost;
        INDIport = indiDlg->INDIport;
        INDICameraName = indiDlg->INDIDevName;
        INDICameraCCD = indiDlg->INDIDevCCD;
        INDICameraPort = indiDlg->INDIDevPort;
        INDICameraForceVideo = indiDlg->INDIForceVideo;
        pConfig->Profile.SetString("/indi/INDIhost", INDIhost);
        pConfig->Profile.SetLong("/indi/INDIport", INDIport);
        pConfig->Profile.SetString("/indi/INDIcam", INDICameraName);
        pConfig->Profile.SetLong("/indi/INDIcam_ccd",INDICameraCCD);
        pConfig->Profile.SetBoolean("/indi/INDIcam_forcevideo",INDICameraForceVideo);
        pConfig->Profile.SetString("/indi/INDIcam_port",INDICameraPort);
        Name = INDICameraName;
        SetCCDdevice();
    }
    indiDlg->Disconnect();
    indiDlg->Destroy();
    delete indiDlg;
}

void  CameraINDI::SetCCDdevice()
{
    if (INDICameraCCD == 0) {
        INDICameraBlobName = "CCD1";
        INDICameraCCDCmd = "CCD_";
    }
    else {
        INDICameraBlobName = "CCD2";
        INDICameraCCDCmd = "GUIDER_";
    }
}

bool CameraINDI::ReadFITS(usImage& img, bool takeSubframe, const wxRect& subframe)
{
    int xsize, ysize;
    fitsfile *fptr;  // FITS file pointer
    int status = 0;  // CFITSIO status value MUST be initialized to zero!
    int hdutype, naxis;
    int nhdus=0;
    long fits_size[2];
    long fpixel[3] = {1,1,1};
    size_t bsize = static_cast<size_t>(cam_bp->bloblen);

    // load blob to CFITSIO
    if (fits_open_memfile(&fptr,
            "",
            READONLY,
            &(cam_bp->blob),
            &bsize,
            0,
            NULL,
            &status) )
    {
        pFrame->Alert(_("Unsupported type or read error loading FITS file"));
        return true;
    }
    if (fits_get_hdu_type(fptr, &hdutype, &status) || hdutype != IMAGE_HDU) {
        pFrame->Alert(_("FITS file is not of an image"));
        PHD_fits_close_file(fptr);
        return true;
    }

    // Get HDUs and size
    fits_get_img_dim(fptr, &naxis, &status);
    fits_get_img_size(fptr, 2, fits_size, &status);
    xsize = (int) fits_size[0];
    ysize = (int) fits_size[1];
    fits_get_num_hdus(fptr,&nhdus,&status);
    if ((nhdus != 1) || (naxis != 2)) {
        pFrame->Alert(_("Unsupported type or read error loading FITS file"));
        PHD_fits_close_file(fptr);
        return true;
    }
    if (takeSubframe) {
        if (img.Init(FullSize)) {
            pFrame->Alert(_("Memory allocation error"));
            PHD_fits_close_file(fptr);
            return true;
        }
        img.Clear();
        img.Subframe = subframe;
        unsigned short *rawdata = new unsigned short[xsize*ysize];
        if (fits_read_pix(fptr, TUSHORT, fpixel, xsize*ysize, NULL, rawdata, NULL, &status) ) {
            pFrame->Alert(_("Error reading data"));
            PHD_fits_close_file(fptr);
            return true;
        }
        int i = 0;
        for (int y = 0; y < subframe.height; y++)
        {
            unsigned short *dataptr = img.ImageData + (y + subframe.y) * img.Size.GetWidth() + subframe.x;
            memcpy(dataptr, &rawdata[i], subframe.width * sizeof(unsigned short));
            i += subframe.width;
        }
        delete[] rawdata;
    }
    else {
        if (img.Init(xsize,ysize)) {
            pFrame->Alert(_("Memory allocation error"));
            PHD_fits_close_file(fptr);
            return true;
        }
        // Read image
        if (fits_read_pix(fptr, TUSHORT, fpixel, xsize*ysize, NULL, img.ImageData, NULL, &status) ) {
            pFrame->Alert(_("Error reading data"));
            PHD_fits_close_file(fptr);
            return true;
        }
    }

    PHD_fits_close_file(fptr);
    return false;
}

bool CameraINDI::ReadStream(usImage& img)
{
    int xsize, ysize;
    unsigned char *inptr;
    unsigned short *outptr;

    if (! frame_prop) {
        pFrame->Alert(_("No CCD_FRAME property, failed to determine image dimensions"));
        return true;
    }

    if (! (frame_width)) {
        pFrame->Alert(_("No WIDTH value, failed to determine image dimensions"));
        return true;
    }
    xsize = frame_width->value;

    if (! (frame_height)) {
        pFrame->Alert(_("No HEIGHT value, failed to determine image dimensions"));
        return true;
    }
    ysize = frame_height->value;

    // allocate image
    if (img.Init(xsize,ysize)) {
        pFrame->Alert(_("CCD stream: memory allocation error"));
        return true;
    }
    // copy image
    outptr = img.ImageData;
    inptr = (unsigned char *) cam_bp->blob;
    for (int i = 0; i < xsize * ysize; i++)
        *outptr ++ = *inptr++;
    return false;
}

bool CameraINDI::StackStream()
{
    unsigned char *inptr;
    unsigned short *outptr;

    if (StackImg)
    {
        // Add new blob to stacked image
        stacking = true;
        outptr = StackImg->ImageData;
        inptr = (unsigned char *) cam_bp->blob;

        for (int i = 0; i < StackImg->NPixels; i++, outptr++, inptr++)
            *outptr = *outptr + (unsigned short) (*inptr);

        StackFrames++;

        stacking = false;
        return false;
    }
    else
    {
        return true;
    }
}

bool CameraINDI::Capture(int duration, usImage& img, int options, const wxRect& subframeArg)
{
  if (Connected) {

      bool takeSubframe = UseSubframes;
      wxRect subframe(subframeArg);

      // we can set the exposure time directly in the camera
      if ((expose_prop)&&(!INDICameraForceVideo)) {
          if (binning_prop && (Binning != m_curBinning))
          {
              FullSize = wxSize(m_maxSize.x / Binning, m_maxSize.y / Binning);
              binning_x->value = Binning;
              binning_y->value = Binning;
              sendNewNumber(binning_prop);
              m_curBinning = Binning;
              takeSubframe = false; // subframe may be out of bounds now
          }

          if (! frame_prop || subframe.width <= 0 || subframe.height <= 0)
          {
              takeSubframe = false;
          }

          // Program the size
          if (!takeSubframe)
          {
              subframe = wxRect(0, 0, FullSize.GetWidth(), FullSize.GetHeight());
          }

          if (frame_prop && (subframe != m_roi))
          {
             frame_x->value = subframe.x*Binning;
             frame_y->value = subframe.y*Binning;
             frame_width->value = subframe.width*Binning;
             frame_height->value = subframe.height*Binning;
             sendNewNumber(frame_prop);
             m_roi = subframe;
          }
          //printf("Exposing for %d(ms)\n", duration);

          // set the exposure time, this immediately start the exposure
          expose_prop->np->value = (double)duration/1000;
          sendNewNumber(expose_prop);

          modal = true;  // will be reset when the image blob is received

          unsigned long loopwait = duration > 100 ? 10 : 1;

          CameraWatchdog watchdog(duration, GetTimeoutMs());

          while (modal) {
             wxMilliSleep(loopwait);
             if (WorkerThread::TerminateRequested())
                return true;
             if (watchdog.Expired())
             {
                if (first_frame && video_prop)
                {
                    // exposure fail, maybe this is a webcam with only streaming
                    // try to use video stream instead of exposure
                    // See: http://www.indilib.org/forum/ccds-dslrs/3078-v4l2-ccd-exposure-property.html
                    // TODO : check if an updated INDI v4l2 driver offer a better solution
                    pFrame->Alert(wxString::Format(_("Camera  %s, exposure error. Trying to use streaming instead."), INDICameraName));
                    INDICameraForceVideo = true;
                    first_frame = false;
                    return Capture(duration, img,  options, subframeArg);
                }
                else{
                   first_frame = false;
                   DisconnectWithAlert(CAPT_FAIL_TIMEOUT);
                   return true;
                }
             }
          }

          //printf("Exposure end\n");
          first_frame = false;

          // exposure complete, process the file
          if (strcmp(cam_bp->format, ".fits") == 0) {
              //printf("Processing fits file\n");
              // for CCD camera
              if ( ! ReadFITS(img,takeSubframe,subframe) ) {
                  if (options & CAPTURE_SUBTRACT_DARK) {
                  //printf("Subtracting dark\n");
                  SubtractDark(img);
                  }
                  if (options & CAPTURE_RECON) {
                      if (PixSizeX != PixSizeY) SquarePixels(img, PixSizeX, PixSizeY);
                  }
                  return false;
              } else {
                  return true;
              }
          } else {
              pFrame->Alert(_("Unknown image format: ") + wxString::FromAscii(cam_bp->format));
              return true;
          }
      }
      // for video camera without exposure time setting we stack frames for duration of the exposure
      else if (video_prop){

          takeSubframe = false;
          first_frame = false;

          if (img.Init(FullSize)) {
             DisconnectWithAlert(CAPT_FAIL_MEMORY);
             return true;
          }
          img.Clear();
          StackImg = &img;

          // Find INDI switch
          ISwitch *v_on;
          ISwitch *v_off;
          if (has_old_videoprop){
             v_on = IUFindSwitch(video_prop,"ON");
             v_off = IUFindSwitch(video_prop,"OFF");
          }
          else {
             v_on = IUFindSwitch(video_prop,"STREAM_ON");
             v_off = IUFindSwitch(video_prop,"STREAM_OFF");
          }

          // start streaming if not already active, every video frame is received as a blob
          if (v_on->s != ISS_ON)
          {
            v_on->s = ISS_ON;
            v_off->s = ISS_OFF;
            sendNewSwitch(video_prop);
          }

          modal = true;
          stacking = false;
          StackFrames = 0;

          unsigned long loopwait = duration > 100 ? 10 : 1;
          wxStopWatch swatch;
          swatch.Start();

          // wait the required time
          while (modal) {
             wxMilliSleep(loopwait);
             // test exposure complete
             if ((swatch.Time() >= duration) && (StackFrames > 2))
                 modal = false;
             // test termination request, stop streaming before to return
             if (WorkerThread::TerminateRequested())
                 modal = false;
          }

          if (WorkerThread::StopRequested() ||  WorkerThread::TerminateRequested())
          {
            // Stop video streaming when Stop button is pressed or exiting the program
            v_on->s = ISS_OFF;
            v_off->s = ISS_ON;
            sendNewSwitch(video_prop);
          }

          if (WorkerThread::TerminateRequested())
              return true;

          // wait current frame is processed
          while (stacking) {
             wxMilliSleep(loopwait);
          }

          pFrame->StatusMsg(wxString::Format(_("%d frames"),StackFrames));

          if (options & CAPTURE_SUBTRACT_DARK) SubtractDark(img);

          return false;

      }
      else {
          // no capture property.
          wxString msg;
          if (INDICameraForceVideo) {
             msg = _("Camera has no VIDEO_STREAM property, please uncheck the option: Camera do not support exposure time.");
          }
          else {
             msg = _("Camera has no CCD_EXPOSURE or VIDEO_STREAM property");
          }
          DisconnectWithAlert(msg,NO_RECONNECT);
          return true;
      }
  }
  else {
      // in case the camera is not connected
      return true;
  }
  // we must never go here
  return true;
}

bool CameraINDI::HasNonGuiCapture(void)
{
    return true;
}

// Camera ST4 port

bool CameraINDI::ST4HasNonGuiMove(void)
{
    return true;
}

bool CameraINDI::ST4PulseGuideScope(int direction, int duration)
{
    if (pulseGuideNS_prop && pulseGuideEW_prop) {
        switch (direction) {
            case EAST:
                pulseE_prop->value = duration;
                pulseW_prop->value = 0;
                sendNewNumber(pulseGuideEW_prop);
                break;
            case WEST:
                pulseE_prop->value = 0;
                pulseW_prop->value = duration;
                sendNewNumber(pulseGuideEW_prop);
                break;
            case NORTH:
                pulseN_prop->value = duration;
                pulseS_prop->value = 0;
                sendNewNumber(pulseGuideNS_prop);
                break;
            case SOUTH:
                pulseN_prop->value = 0;
                pulseS_prop->value = duration;
                sendNewNumber(pulseGuideNS_prop);
                break;
            case NONE:
                Debug.Write("error CameraINDI::Guide NONE\n");
                break;
        }
        wxMilliSleep(duration);
        return false;
    }
    else return true;
}

bool CameraINDI::GetDevicePixelSize(double *devPixelSize)
{
    if (!Connected)
        return true; // error

    *devPixelSize = PixSize;
    return false;
}

#endif
