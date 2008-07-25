/* simDetector.cpp
 *
 * This is a driver for a simulated area detector.
 *
 * Author: Mark Rivers
 *         University of Chicago
 *
 * Created:  March 20, 2008
 *
 */
 
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <epicsStdio.h>
#include <epicsMutex.h>
#include <cantProceed.h>

#include "ADStdDriverParams.h"
#include "NDArray.h"
#include "ADDriver.h"

#include "drvSimDetector.h"


static const char *driverName = "drvSimDetector";

class simDetector : public ADDriver {
public:
    simDetector(const char *portName, int maxSizeX, int maxSizeY, NDDataType_t dataType,
                int maxBuffers, size_t maxMemory);
                 
    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual asynStatus writeFloat64(asynUser *pasynUser, epicsFloat64 value);
    virtual asynStatus drvUserCreate(asynUser *pasynUser, const char *drvInfo, 
                                     const char **pptypeName, size_t *psize);
    void report(FILE *fp, int details);
                                        
    /* These are the methods that are new to this class */
    template <typename epicsType> int computeArray(int maxSizeX, int maxSizeY);
    int allocateBuffer();
    int computeImage();
    void simTask();

    /* Our data */
    int imagesRemaining;
    epicsEventId startEventId;
    epicsEventId stopEventId;
    NDArray *pRaw;
};

/* If we have any private driver parameters they begin with ADFirstDriverParam and should end
   with ADLastDriverParam, which is used for setting the size of the parameter library table */
typedef enum {
    SimGainX 
        = ADFirstDriverParam,
    SimGainY,
    SimResetImage,
    ADLastDriverParam
} SimDetParam_t;

static asynParamString_t SimDetParamString[] = {
    {SimGainX,          "SIM_GAINX"},  
    {SimGainY,          "SIM_GAINY"},  
    {SimResetImage,     "RESET_IMAGE"},  
};

#define NUM_SIM_DET_PARAMS (sizeof(SimDetParamString)/sizeof(SimDetParamString[0]))

template <typename epicsType> int simDetector::computeArray(int maxSizeX, int maxSizeY)
{
    epicsType *pData = (epicsType *)this->pRaw->pData;
    epicsType inc;
    int addr=0;
    int status = asynSuccess;
    double scaleX=0., scaleY=0.;
    double exposureTime, gain, gainX, gainY;
    int resetImage;
    int i, j;

    status = getDoubleParam (addr, ADGain,        &gain);
    status = getDoubleParam (addr, SimGainX,      &gainX);
    status = getDoubleParam (addr, SimGainY,      &gainY);
    status = getIntegerParam(addr, SimResetImage, &resetImage);
    status = getDoubleParam (addr, ADAcquireTime, &exposureTime);

    /* The intensity at each pixel[i,j] is:
     * (i * gainX + j* gainY) + imageCounter * gain * exposureTime * 1000. */
    inc = (epicsType) (gain * exposureTime * 1000.);

    if (resetImage) {
        for (i=0; i<maxSizeY; i++) {
            scaleX = 0.;
            for (j=0; j<maxSizeX; j++) {
                (*pData++) = (epicsType)(scaleX + scaleY + inc);
                scaleX += gainX;
            }
            scaleY += gainY;
        }
    } else {
        for (i=0; i<maxSizeY; i++) {
            for (j=0; j<maxSizeX; j++) {
                 *pData++ += inc;
            }
        }
    }
    return(status);
}


int simDetector::allocateBuffer()
{
    int status = asynSuccess;
    NDArrayInfo_t arrayInfo;
    
    /* Make sure the raw array we have allocated is large enough. 
     * We are allowed to change its size because we have exclusive use of it */
    this->pRaw->getInfo(&arrayInfo);
    if (arrayInfo.totalBytes > this->pRaw->dataSize) {
        free(this->pRaw->pData);
        this->pRaw->pData  = malloc(arrayInfo.totalBytes);
        this->pRaw->dataSize = arrayInfo.totalBytes;
        if (!this->pRaw->pData) status = asynError;
    }
    return(status);
}

int simDetector::computeImage()
{
    int status = asynSuccess;
    NDDataType_t dataType;
    int addr=0;
    int binX, binY, minX, minY, sizeX, sizeY, reverseX, reverseY;
    int maxSizeX, maxSizeY;
    NDDimension_t dimsOut[2];
    NDArrayInfo_t arrayInfo;
    NDArray *pImage;
    const char* functionName = "computeImage";

    /* NOTE: The caller of this function must have taken the mutex */
    
    status |= getIntegerParam(addr, ADBinX,         &binX);
    status |= getIntegerParam(addr, ADBinY,         &binY);
    status |= getIntegerParam(addr, ADMinX,         &minX);
    status |= getIntegerParam(addr, ADMinY,         &minY);
    status |= getIntegerParam(addr, ADSizeX,        &sizeX);
    status |= getIntegerParam(addr, ADSizeY,        &sizeY);
    status |= getIntegerParam(addr, ADReverseX,     &reverseX);
    status |= getIntegerParam(addr, ADReverseY,     &reverseY);
    status |= getIntegerParam(addr, ADMaxSizeX,     &maxSizeX);
    status |= getIntegerParam(addr, ADMaxSizeY,     &maxSizeY);
    status |= getIntegerParam(addr, ADDataType,     (int *)&dataType);
    if (status) asynPrint(this->pasynUser, ASYN_TRACE_ERROR,
                    "%s:%s: error getting parameters\n",
                    driverName, functionName);

    /* Make sure parameters are consistent, fix them if they are not */
    if (binX < 1) {
        binX = 1; 
        status |= setIntegerParam(addr, ADBinX, binX);
    }
    if (binY < 1) {
        binY = 1;
        status |= setIntegerParam(addr, ADBinY, binY);
    }
    if (minX < 0) {
        minX = 0; 
        status |= setIntegerParam(addr, ADMinX, minX);
    }
    if (minY < 0) {
        minY = 0; 
        status |= setIntegerParam(addr, ADMinY, minY);
    }
    if (minX > maxSizeX-1) {
        minX = maxSizeX-1; 
        status |= setIntegerParam(addr, ADMinX, minX);
    }
    if (minY > maxSizeY-1) {
        minY = maxSizeY-1; 
        status |= setIntegerParam(addr, ADMinY, minY);
    }
    if (minX+sizeX > maxSizeX) {
        sizeX = maxSizeX-minX; 
        status |= setIntegerParam(addr, ADSizeX, sizeX);
    }
    if (minY+sizeY > maxSizeY) {
        sizeY = maxSizeY-minY; 
        status |= setIntegerParam(addr, ADSizeY, sizeY);
    }

    /* Make sure the buffer we have allocated is large enough. */
    this->pRaw->dataType = dataType;
    status = allocateBuffer();
    if (status) {
        asynPrint(this->pasynUser, ASYN_TRACE_ERROR,
                  "%s:%s: error allocating raw buffer\n",
                  driverName, functionName);
        return(status);
    }
    switch (dataType) {
        case NDInt8: 
            status |= computeArray<epicsInt8>(maxSizeX, maxSizeY);
            break;
        case NDUInt8: 
            status |= computeArray<epicsUInt8>(maxSizeX, maxSizeY);
            break;
        case NDInt16: 
            status |= computeArray<epicsInt16>(maxSizeX, maxSizeY);
            break;
        case NDUInt16: 
            status |= computeArray<epicsUInt16>(maxSizeX, maxSizeY);
            break;
        case NDInt32: 
            status |= computeArray<epicsInt32>(maxSizeX, maxSizeY);
            break;
        case NDUInt32: 
            status |= computeArray<epicsUInt32>(maxSizeX, maxSizeY);
            break;
        case NDFloat32: 
            status |= computeArray<epicsFloat32>(maxSizeX, maxSizeY);
            break;
        case NDFloat64: 
            status |= computeArray<epicsFloat64>(maxSizeX, maxSizeY);
            break;
    }
    
    /* Extract the region of interest with binning.  
     * If the entire image is being used (no ROI or binning) that's OK because
     * convertImage detects that case and is very efficient */
    this->pRaw->initDimension(&dimsOut[0], sizeX);
    dimsOut[0].binning = binX;
    dimsOut[0].offset = minX;
    dimsOut[0].reverse = reverseX;
    this->pRaw->initDimension(&dimsOut[1], sizeY);
    dimsOut[1].binning = binY;
    dimsOut[1].offset = minY;
    dimsOut[1].reverse = reverseY;
    /* We save the most recent image buffer so it can be used in the read() function.
     * Now release it before getting a new version. */
    if (this->pArrays[addr]) this->pArrays[addr]->release();
    status = this->pNDArrayPool->convert(this->pRaw,
                                         &this->pArrays[addr],
                                         dataType,
                                         dimsOut);
    if (status) {
        asynPrint(this->pasynUser, ASYN_TRACE_ERROR,
                    "%s:%s: error allocating buffer in convert()\n",
                    driverName, functionName);
        return(status);
    }
    pImage = this->pArrays[addr];
    pImage->getInfo(&arrayInfo);
    status = asynSuccess;
    status |= setIntegerParam(addr, ADImageSize,  arrayInfo.totalBytes);
    status |= setIntegerParam(addr, ADImageSizeX, pImage->dims[0].size);
    status |= setIntegerParam(addr, ADImageSizeY, pImage->dims[1].size);
    status |= setIntegerParam(addr, SimResetImage, 0);
    if (status) asynPrint(this->pasynUser, ASYN_TRACE_ERROR,
                    "%s:%s: error setting parameters\n",
                    driverName, functionName);
    return(status);
}

static void simTaskC(void *drvPvt)
{
    simDetector *pPvt = (simDetector *)drvPvt;
    
    pPvt->simTask();
}

void simDetector::simTask()
{
    /* This thread computes new image data and does the callbacks to send it to higher layers */
    int status = asynSuccess;
    int dataType;
    int addr=0;
    int imageSizeX, imageSizeY, imageSize;
    int imageCounter;
    int acquire, autoSave;
    ADStatus_t acquiring;
    NDArray *pImage;
    double acquireTime, acquirePeriod, delay;
    epicsTimeStamp startTime, endTime;
    double elapsedTime;
    const char *functionName = "simTask";

    /* Loop forever */
    while (1) {
    
        epicsMutexLock(this->mutexId);

        /* Is acquisition active? */
        getIntegerParam(addr, ADAcquire, &acquire);
        
        /* If we are not acquiring then wait for a semaphore that is given when acquisition is started */
        if (!acquire) {
            setIntegerParam(addr, ADStatus, ADStatusIdle);
            callParamCallbacks(addr, addr);
            /* Release the lock while we wait for an event that says acquire has started, then lock again */
            epicsMutexUnlock(this->mutexId);
            asynPrint(this->pasynUser, ASYN_TRACE_FLOW, 
                "%s:%s: waiting for acquire to start\n", driverName, functionName);
            status = epicsEventWait(this->startEventId);
            epicsMutexLock(this->mutexId);
        }
        
        /* We are acquiring. */
        /* Get the current time */
        epicsTimeGetCurrent(&startTime);
        
        /* Get the exposure parameters */
        getDoubleParam(addr, ADAcquireTime, &acquireTime);
        getDoubleParam(addr, ADAcquirePeriod, &acquirePeriod);
        
        acquiring = ADStatusAcquire;
        setIntegerParam(addr, ADStatus, acquiring);

        /* Call the callbacks to update any changes */
        callParamCallbacks(addr, addr);

        /* Simulate being busy during the exposure time.  Use epicsEventWaitWithTimeout so that
         * manually stopping the acquisition will work */
        if (acquireTime >= epicsThreadSleepQuantum()) {
            epicsMutexUnlock(this->mutexId);
            status = epicsEventWaitWithTimeout(this->stopEventId, acquireTime);
            epicsMutexLock(this->mutexId);
        }
        
        /* Update the image */
        status = computeImage();
        if (status) {
            epicsMutexUnlock(this->mutexId);
            continue;
        }
            
        pImage = this->pArrays[addr];
        
        epicsTimeGetCurrent(&endTime);
        elapsedTime = epicsTimeDiffInSeconds(&endTime, &startTime);

        /* Get the current parameters */
        getIntegerParam(addr, ADImageSizeX, &imageSizeX);
        getIntegerParam(addr, ADImageSizeY, &imageSizeY);
        getIntegerParam(addr, ADImageSize,  &imageSize);
        getIntegerParam(addr, ADDataType,   &dataType);
        getIntegerParam(addr, ADAutoSave,   &autoSave);
        getIntegerParam(addr, ADImageCounter, &imageCounter);
        imageCounter++;
        setIntegerParam(addr, ADImageCounter, imageCounter);
        
        /* Put the frame number and time stamp into the buffer */
        pImage->uniqueId = imageCounter;
        pImage->timeStamp = startTime.secPastEpoch + startTime.nsec / 1.e9;
        
        /* Call the NDArray callback */
        /* Must release the lock here, or we can get into a deadlock, because we can
         * block on the plugin lock, and the plugin can be calling us */
        epicsMutexUnlock(this->mutexId);
        asynPrint(this->pasynUser, ASYN_TRACE_FLOW, 
             "%s:%s: calling imageData callback\n", driverName, functionName);
        doCallbacksGenericPointer(pImage, NDArrayData, addr);
        epicsMutexLock(this->mutexId);

        /* See if acquisition is done */
        if (this->imagesRemaining > 0) this->imagesRemaining--;
        if (this->imagesRemaining == 0) {
            acquiring = ADStatusIdle;
            setIntegerParam(addr, ADAcquire, acquiring);
            asynPrint(this->pasynUser, ASYN_TRACE_FLOW, 
                  "%s:%s: acquisition completed\n", driverName, functionName);
        }
        
        /* Call the callbacks to update any changes */
        callParamCallbacks(addr, addr);
        
        
        /* If we are acquiring then sleep for the acquire period minus elapsed time. */
        if (acquiring) {
            /* We set the status to readOut to indicate we are in the period delay */
            setIntegerParam(addr, ADStatus, ADStatusReadout);
            callParamCallbacks(addr, addr);
            /* We are done accessing data structures, release the lock */
            epicsMutexUnlock(this->mutexId);
            delay = acquirePeriod - elapsedTime;
            asynPrint(this->pasynUser, ASYN_TRACE_FLOW, 
                     "%s:%s: delay=%f\n",
                      driverName, functionName, delay);            
            if (delay >= epicsThreadSleepQuantum())
                status = epicsEventWaitWithTimeout(this->stopEventId, delay);

        } else {
            /* We are done accessing data structures, release the lock */
            epicsMutexUnlock(this->mutexId);
        }
    }
}


asynStatus simDetector::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    int adstatus;
    int addr=0;
    asynStatus status = asynSuccess;

    /* Set the parameter and readback in the parameter library.  This may be overwritten when we read back the
     * status at the end, but that's OK */
    status = setIntegerParam(addr, function, value);

    /* For a real detector this is where the parameter is sent to the hardware */
    switch (function) {
    case ADAcquire:
        getIntegerParam(addr, ADStatus, &adstatus);
        if (value && (adstatus == ADStatusIdle)) {
            /* We need to set the number of images we expect to collect, so the image callback function
               can know when acquisition is complete.  We need to find out what mode we are in and how
               many images have been requested.  If we are in continuous mode then set the number of
               remaining images to -1. */
            int imageMode, numImages;
            status = getIntegerParam(addr, ADImageMode, &imageMode);
            status = getIntegerParam(addr, ADNumImages, &numImages);
            switch(imageMode) {
            case ADImageSingle:
                this->imagesRemaining = 1;
                break;
            case ADImageMultiple:
                this->imagesRemaining = numImages;
                break;
            case ADImageContinuous:
                this->imagesRemaining = -1;
                break;
            }
            /* Send an event to wake up the simulation task.  
             * It won't actually start generating new images until we release the lock below */
            epicsEventSignal(this->startEventId);
        } 
        if (!value && (adstatus != ADStatusIdle)) {
            /* This was a command to stop acquisition */
            /* Send the stop event */
            epicsEventSignal(this->stopEventId);
        }
        break;
    case ADBinX:
    case ADBinY:
    case ADMinX:
    case ADMinY:
    case ADSizeX:
    case ADSizeY:
    case ADDataType:
        status = setIntegerParam(addr, SimResetImage, 1);
        break;
    case ADImageMode: 
        /* The image mode may have changed while we are acquiring, 
         * set the images remaining appropriately. */
        switch (value) {
        case ADImageSingle:
            this->imagesRemaining = 1;
            break;
        case ADImageMultiple: {
            int numImages;
            getIntegerParam(addr, ADNumImages, &numImages);
            this->imagesRemaining = numImages; }
            break;
        case ADImageContinuous:
            this->imagesRemaining = -1;
            break;
        }
        break;
    }
    
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks(addr, addr);
    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:writeInt32 error, status=%d function=%d, value=%d\n", 
              driverName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:writeInt32: function=%d, value=%d\n", 
              driverName, function, value);
    return status;
}


asynStatus simDetector::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    int addr=0;

    /* Set the parameter and readback in the parameter library.  This may be overwritten when we read back the
     * status at the end, but that's OK */
    status = setDoubleParam(addr, function, value);

    /* Changing any of the following parameters requires recomputing the base image */
    switch (function) {
    case ADAcquireTime:
    case ADGain:
    case SimGainX:
    case SimGainY:
        status = setIntegerParam(addr, SimResetImage, 1);
        break;
    }

    /* Do callbacks so higher layers see any changes */
    callParamCallbacks(addr, addr);
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:writeFloat64 error, status=%d function=%d, value=%f\n", 
              driverName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:writeFloat64: function=%d, value=%f\n", 
              driverName, function, value);
    return status;
}


/* asynDrvUser routines */
asynStatus simDetector::drvUserCreate(asynUser *pasynUser,
                                      const char *drvInfo, 
                                      const char **pptypeName, size_t *psize)
{
    asynStatus status;
    int param;
    const char *functionName = "drvUserCreate";

    /* See if this is one of our standard parameters */
    status = findParam(SimDetParamString, NUM_SIM_DET_PARAMS, 
                       drvInfo, &param);
                                
    if (status == asynSuccess) {
        pasynUser->reason = param;
        if (pptypeName) {
            *pptypeName = epicsStrDup(drvInfo);
        }
        if (psize) {
            *psize = sizeof(param);
        }
        asynPrint(pasynUser, ASYN_TRACE_FLOW,
                  "%s:%s: drvInfo=%s, param=%d\n", 
                  driverName, functionName, drvInfo, param);
        return(asynSuccess);
    }
    
    /* If not, then see if it is a base class parameter */
    status = ADDriver::drvUserCreate(pasynUser, drvInfo, pptypeName, psize);
    return(status);  
}
    
void simDetector::report(FILE *fp, int details)
{
    int addr=0;

    fprintf(fp, "Simulation detector %s\n", this->portName);
    if (details > 0) {
        int nx, ny, dataType;
        getIntegerParam(addr, ADSizeX, &nx);
        getIntegerParam(addr, ADSizeY, &ny);
        getIntegerParam(addr, ADDataType, &dataType);
        fprintf(fp, "  NX, NY:            %d  %d\n", nx, ny);
        fprintf(fp, "  Data type:         %d\n", dataType);
    }
    /* Invoke the base class method */
    ADDriver::report(fp, details);
}

extern "C" int simDetectorConfig(const char *portName, int maxSizeX, int maxSizeY, int dataType,
                                 int maxBuffers, size_t maxMemory)
{
    new simDetector(portName, maxSizeX, maxSizeY, (NDDataType_t)dataType, maxBuffers, maxMemory);
    return(asynSuccess);
}

simDetector::simDetector(const char *portName, int maxSizeX, int maxSizeY, NDDataType_t dataType,
                         int maxBuffers, size_t maxMemory)

    : ADDriver(portName, 1, ADLastDriverParam, maxBuffers, maxMemory, 0, 0), 
      imagesRemaining(0), pRaw(NULL)

{
    int status = asynSuccess;
    const char *functionName = "simDetector";
    int addr=0;
    int dims[2];

    /* Create the epicsEvents for signaling to the simulate task when acquisition starts and stops */
    this->startEventId = epicsEventCreate(epicsEventEmpty);
    if (!this->startEventId) {
        printf("%s:%s epicsEventCreate failure for start event\n", 
            driverName, functionName);
        return;
    }
    this->stopEventId = epicsEventCreate(epicsEventEmpty);
    if (!this->stopEventId) {
        printf("%s:%s epicsEventCreate failure for stop event\n", 
            driverName, functionName);
        return;
    }
    
    /* Allocate the raw buffer we use to compute images.  Only do this once */
    dims[0] = maxSizeX;
    dims[1] = maxSizeY;
    this->pRaw = this->pNDArrayPool->alloc(2, dims, dataType, 0, NULL);

    /* Set some default values for parameters */
    status =  setStringParam (addr, ADManufacturer, "Simulated detector");
    status |= setStringParam (addr, ADModel, "Basic simulator");
    status |= setIntegerParam(addr, ADMaxSizeX, maxSizeX);
    status |= setIntegerParam(addr, ADMaxSizeY, maxSizeY);
    status |= setIntegerParam(addr, ADSizeX, maxSizeX);
    status |= setIntegerParam(addr, ADSizeX, maxSizeX);
    status |= setIntegerParam(addr, ADSizeY, maxSizeY);
    status |= setIntegerParam(addr, ADImageSizeX, maxSizeX);
    status |= setIntegerParam(addr, ADImageSizeY, maxSizeY);
    status |= setIntegerParam(addr, ADImageSize, 0);
    status |= setIntegerParam(addr, ADDataType, dataType);
    status |= setIntegerParam(addr, ADImageMode, ADImageContinuous);
    status |= setDoubleParam (addr, ADAcquireTime, .001);
    status |= setDoubleParam (addr, ADAcquirePeriod, .005);
    status |= setIntegerParam(addr, ADNumImages, 100);
    status |= setIntegerParam(addr, SimResetImage, 1);
    status |= setDoubleParam (addr, SimGainX, 1);
    status |= setDoubleParam (addr, SimGainY, 1);
    if (status) {
        printf("%s: unable to set camera parameters\n", functionName);
        return;
    }
    
    /* Create the thread that updates the images */
    status = (epicsThreadCreate("SimDetTask",
                                epicsThreadPriorityMedium,
                                epicsThreadGetStackSize(epicsThreadStackMedium),
                                (EPICSTHREADFUNC)simTaskC,
                                this) == NULL);
    if (status) {
        printf("%s:%s epicsThreadCreate failure for image task\n", 
            driverName, functionName);
        return;
    }
}