/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This file contains functions interacting with the CoreAudio framework
 * that are not specific to the AUHAL. These are split in a separate file for
 * the sake of readability. In the future the could be used by other AOs based
 * on CoreAudio but not the AUHAL (such as using AudioQueue services).
 */

#include "audio/out/ao_coreaudio_utils.h"
#include "osdep/timer.h"
#include "osdep/endian.h"
#include "osdep/semaphore.h"
#include "audio/format.h"

#if HAVE_COREAUDIO
#include "audio/out/ao_coreaudio_properties.h"
#include <CoreAudio/HostTime.h>
#else
#include <mach/mach_time.h>
#endif

CFStringRef cfstr_from_cstr(char *str)
{
    return CFStringCreateWithCString(NULL, str, CA_CFSTR_ENCODING);
}

char *cfstr_get_cstr(CFStringRef cfstr)
{
    CFIndex size =
        CFStringGetMaximumSizeForEncoding(
            CFStringGetLength(cfstr), CA_CFSTR_ENCODING) + 1;
    char *buffer = talloc_zero_size(NULL, size);
    CFStringGetCString(cfstr, buffer, size, CA_CFSTR_ENCODING);
    return buffer;
}

#if HAVE_COREAUDIO
static bool ca_is_output_device(struct ao *ao, AudioDeviceID dev)
{
    size_t n_buffers;
    AudioBufferList *buffers;
    const ca_scope scope = kAudioDevicePropertyStreamConfiguration;
    OSStatus err = CA_GET_ARY_O(dev, scope, &buffers, &n_buffers);
    if (err != noErr)
        return false;
    talloc_free(buffers);
    return n_buffers > 0;
}

void ca_get_device_list(struct ao *ao, struct ao_device_list *list)
{
    AudioDeviceID *devs;
    size_t n_devs;
    OSStatus err =
        CA_GET_ARY(kAudioObjectSystemObject, kAudioHardwarePropertyDevices,
                   &devs, &n_devs);
    CHECK_CA_ERROR("Failed to get list of output devices.");
    for (int i = 0; i < n_devs; i++) {
        if (!ca_is_output_device(ao, devs[i]))
            continue;
        void *ta_ctx = talloc_new(NULL);
        char *name;
        char *desc;
        err = CA_GET_STR(devs[i], kAudioDevicePropertyDeviceUID, &name);
        if (err != noErr) {
            MP_VERBOSE(ao, "skipping device %d, which has no UID\n", i);
            talloc_free(ta_ctx);
            continue;
        }
        talloc_steal(ta_ctx, name);
        err = CA_GET_STR(devs[i], kAudioObjectPropertyName, &desc);
        if (err != noErr)
            desc = talloc_strdup(NULL, "Unknown");
        talloc_steal(ta_ctx, desc);
        ao_device_list_add(list, ao, &(struct ao_device_desc){name, desc});
        talloc_free(ta_ctx);
    }
    talloc_free(devs);
coreaudio_error:
    return;
}

// Optimisation. 
// Increase CoreAudio Frame Buffer Size (reduce CPU load) for 2 benefits, 
    // energy saving and (potentially) improve sound quality.

// On my system (build-in and USB DAC), Frame Buffer Size in float mode can go up to device's maximum value,
    // while that in integer mode can only go to 2043 frames (larger than that will course clicks/distortion). 

//The actual required Frame Buffer Size range can be different from the device's hardware Frame Size range,
    //e.g., my built-in device's hardware Frame Size range is "14-4096 frames", but the minimum Frame Size
    //to play a Hi-res audio (24/96) is "29 frames" and CoreAudio will automatically increase the size. 

//The actual Frame Buffer Size is retrieved from "Latency property fsiz". 

//Setting the Buffer Size beyond the Frame Size range is still playable, as summarised in the following table.
    //Senario                     Frame Size we set here    The actual Frame Buffer Size
    //Play 16/44.1 audio                 10                          14
    //Play 16/44.1 audio                 5000                        4096
    //Play 24/96 Audio                   14                          29

//Increasing CoreAudio Frame Buffer Size will also increase audio latency, but WILL NOT effect A/V sync (At least I don't notice any).



//TO DO: make CoreAudio Frame Buffer Size a mpv property and can be configurable for different devices.

OSStatus ca_get_frame_buffer_size(struct ao *ao, AudioDeviceID device, int *buffersize)
//OSStatus ca_TerminalType(struct ao *ao, AudioDeviceID *device, char *terminalType)
{
    AudioValueRange value_range = {0, 0};
    OSStatus err = CA_GET_O(device, kAudioDevicePropertyBufferFrameSizeRange, &value_range);
    MP_VERBOSE(ao, "Frame buffer size: %.0f to %.0f frames\n", value_range.mMinimum, value_range.mMaximum);
    return err;
}

OSStatus ca_get_Device_Transport_Type(struct ao *ao, AudioDeviceID device)
//OSStatus ca_TerminalType(struct ao *ao, AudioDeviceID *device, char *terminalType)
{
  int Transport_Type;
  OSStatus err = CA_GET_O(device, kAudioDevicePropertyTransportType, &Transport_Type);

  int Source;
  OSStatus err1 = CA_GET_O(device, kAudioDevicePropertyDataSource, &Source);

  if (err == noErr) {
      if (err1 == noErr)
      {    
        MP_VERBOSE(ao, "Device transport type: %s, data source: %s\n", mp_tag_str(CFSwapInt32HostToBig(Transport_Type)),
        mp_tag_str(CFSwapInt32HostToBig(Source)));
        //"kAudioDevicePropertyDataSource" only works for limited Transport Type, e.g. not work for USB or Bluetooth connection.
        }else{
        MP_VERBOSE(ao, "Device transport type: %s\n", mp_tag_str(CFSwapInt32HostToBig(Transport_Type)));
        
      }
  }
    return err & err1;
}

OSStatus ca_set_frame_buffer_size(struct ao *ao, AudioDeviceID device, int *buffersize)
{
    //Reference: https://github.com/cmus/cmus/blob/master/op/coreaudio.c
  //OSStatus err3 = ca_get_frame_buffer_size(ao, device, buffersize);
  //return err3;

    //Automatically set buffersize for different devices (connections).
    
  AudioValueRange value_range = {0, 0};
  OSStatus err = CA_GET_O(device, kAudioDevicePropertyBufferFrameSizeRange, &value_range);
  MP_VERBOSE(ao, "Frame buffer size: %.0f to %.0f frames\n", value_range.mMinimum, value_range.mMaximum);
    //MP_VERBOSE(ao, "Frame Buffer Size: %.0f to %.0f frames\n", value_range.mMinimum, value_range.mMaximum);


    //Reference: https://www.techonthenet.com/c_language/standard_library_functions/string_h/strncmp.php

OSStatus err1 = CA_SET(device, kAudioDevicePropertyBufferFrameSize, buffersize);

if ((*buffersize >= value_range.mMinimum) & (*buffersize <= value_range.mMaximum )){
    MP_VERBOSE(ao, "Set frame buffer size to %d frames\n", *buffersize);
  }else if (*buffersize < value_range.mMinimum){
    *buffersize = value_range.mMinimum;
    MP_VERBOSE(ao, "Target frame size it invalid, set to %d (min) frames\n", *buffersize); 
  }else{
    *buffersize = value_range.mMaximum;
    MP_VERBOSE(ao, "Target frame size it invalid, set to %d (max) frames\n", *buffersize); 
  }


    
    //MP_VERBOSE(ao, "Set frame buffer size to %d frames\n", *buffersize);

    return err & err1;

}

OSStatus ca_get_Terminal_Type(struct ao *ao, AudioDeviceID device)
//OSStatus ca_TerminalType(struct ao *ao, AudioDeviceID *device, char *terminalType)
{
   if (!device)
    return 0;

  UInt32 val = 0;
  UInt32 size = sizeof(UInt32);

  AudioObjectPropertyAddress propertyAddress;
  propertyAddress.mScope    = kAudioObjectPropertyScopeGlobal;
  propertyAddress.mElement  = kAudioObjectPropertyElementMaster;
  propertyAddress.mSelector = kAudioStreamPropertyTerminalType;

  OSStatus ret = AudioObjectGetPropertyData(device, &propertyAddress, 0, NULL, &size, &val);
  if (ret == noErr) {
    MP_VERBOSE(ao, "Stream Terminal Type: %s\n", mp_tag_str(CFSwapInt32HostToBig(val)));
  }
  return ret;
}

// Optimization. Another way to set largest CoreAudio Frame Buffer Size.
// Reference: https://developer.apple.com/library/archive/technotes/tn2321/_index.html

 OSStatus SetAudioPowerHintToFavorSavingPower(void)
{
    AudioObjectPropertyAddress theAddress = { kAudioHardwarePropertyPowerHint,
                                              kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMaster };
 
    UInt32 thePowerHint = kAudioHardwarePowerHintFavorSavingPower;
    return AudioObjectSetPropertyData(kAudioObjectSystemObject,
                                    &theAddress,
                                    0,
                                    NULL,
                                     sizeof(UInt32), &thePowerHint);
}

// Optimisation. 
// According to BitPerfect, reducing IOCycleUsage may improve audio quality. 

// The default value is 1. Lower than 1 will cause a/v sync problems (The audio will play ahead of video). 
    // ONLY use for playing music.

// Extreme low value could lead to audio corruption.
    // (check whether there is "skipping cycle due to overload" message in Console). 

// Lower CoreAudio Frame Buffer Size requires higher IOCycleUsage.
    // In my system, 0.03 is safe for 24/48, 0.15 for 24/96 with I/O Buffer Size of 2043 Frame; 
    // 0.05 for 24/48 with I/O Buffer Size of 1024 Frame.


//TO DO: make CoreAudio IO Cycle Usage a mpv property and can be configurable for different devices.

OSStatus ca_IO_Cycle_Usage(struct ao *ao, AudioDeviceID device, Float32 *IOCycleUsage)
{
   OSStatus err = CA_SET(device, kAudioDevicePropertyIOCycleUsage, IOCycleUsage);
   MP_VERBOSE(ao, "Set audio IO Cycle Usage to %g\n", *IOCycleUsage);
   return err;
}



OSStatus ca_get_ao_volume(struct ao *ao, AudioDeviceID device, UInt32 channel)
{
//UInt32 channel = ao->channels.num; // Channel 0  is master, if available
if (!device)
return 0;
Float32 volume;
Float32 volumedb;
UInt32 dataSize = sizeof(volume);

Float32 VirtualMasterBalance;
Float32 VirtualMasterVolume;
Float32 SubVolumeScalar;
Float32 SubVolumeDecibels;
OSStatus VirtualMasterVolumeresult = CA_GET_O(device, kAudioHardwareServiceDeviceProperty_VirtualMasterVolume, &VirtualMasterVolume);
OSStatus VirtualMasterBalanceresult = CA_GET_O(device, kAudioHardwareServiceDeviceProperty_VirtualMasterBalance, &VirtualMasterBalance);
OSStatus subvloume = CA_GET_O(device, kAudioDevicePropertySubVolumeScalar, &SubVolumeScalar);
OSStatus subvloumedb = CA_GET_O(device, kAudioDevicePropertySubVolumeDecibels, &SubVolumeDecibels);

if (VirtualMasterVolumeresult == noErr){
MP_VERBOSE(ao, "Virtual master volume: %.2f\n", VirtualMasterVolume);
}

if (VirtualMasterBalanceresult == noErr){
MP_VERBOSE(ao, "Virtual master balance: %g\n", VirtualMasterBalance);
}

if ((subvloume == noErr) && (subvloumedb == noErr)) {
MP_VERBOSE(ao, "LFE volume: %.2f (%.1fdB)\n", SubVolumeScalar, SubVolumeDecibels); 
}   

for (UInt32 j = 0; j <= channel;j++) {

    AudioObjectPropertyAddress prop = { 
        kAudioDevicePropertyVolumeScalar, 
        kAudioDevicePropertyScopeOutput,
        j
    };

    AudioObjectPropertyAddress prop_db = { 
        kAudioDevicePropertyVolumeDecibels, 
        kAudioDevicePropertyScopeOutput,
        j
    };
  

    if (AudioObjectHasProperty(device, &prop)){

    OSStatus VolumeScalar = AudioObjectGetPropertyData(device, &prop, 0, NULL, &dataSize, &volume);
    OSStatus VolumeDecibels = AudioObjectGetPropertyData(device, &prop_db, 0, NULL, &dataSize, &volumedb); 

        if ((VolumeScalar == noErr) && (VolumeDecibels == noErr)){
            if (j == 0){
            MP_VERBOSE(ao, "Master volume: %.2f (%.1fdB)\n", volume, volumedb);
            }else{
            MP_VERBOSE(ao, "Channel %u volume: %.2f (%.1fdB)\n", j, volume, volumedb); 
            }
        }
    }
}

return noErr;
}

OSStatus ca_select_device(struct ao *ao, char* name, AudioDeviceID *device)
{
    OSStatus err = noErr;
    *device = kAudioObjectUnknown;

    if (name && name[0]) {
        CFStringRef uid = cfstr_from_cstr(name);
        AudioValueTranslation v = (AudioValueTranslation) {
            .mInputData = &uid,
            .mInputDataSize = sizeof(CFStringRef),
            .mOutputData = device,
            .mOutputDataSize = sizeof(*device),
        };
        uint32_t size = sizeof(AudioValueTranslation);
        AudioObjectPropertyAddress p_addr = (AudioObjectPropertyAddress) {
            .mSelector = kAudioHardwarePropertyDeviceForUID,
            .mScope    = kAudioObjectPropertyScopeGlobal,
            .mElement  = kAudioObjectPropertyElementMaster,
        };
        err = AudioObjectGetPropertyData(
            kAudioObjectSystemObject, &p_addr, 0, 0, &size, &v);
        CFRelease(uid);
        CHECK_CA_ERROR("unable to query for device UID");

        uint32_t is_alive = 1;
        err = CA_GET(*device, kAudioDevicePropertyDeviceIsAlive, &is_alive);
        CHECK_CA_ERROR("could not check whether device is alive (invalid device?)");

        if (!is_alive)
            MP_WARN(ao, "device is not alive!\n");
    } else {
        // device not set by user, get the default one
        err = CA_GET(kAudioObjectSystemObject,
                     kAudioHardwarePropertyDefaultOutputDevice,
                     device);
        CHECK_CA_ERROR("could not get default audio device");
    }

    if (mp_msg_test(ao->log, MSGL_V)) {
        char *desc;
        OSStatus err2 = CA_GET_STR(*device, kAudioObjectPropertyName, &desc);
        if (err2 == noErr) {
            MP_VERBOSE(ao, "Selected audio output device: %s (0x%02x)\n",
                           desc, *device);
            talloc_free(desc);
        }
    }

coreaudio_error:
    return err;
}
#endif

bool check_ca_st(struct ao *ao, int level, OSStatus code, const char *message)
{
    if (code == noErr) return true;

    mp_msg(ao->log, level, "%s (%s/%d)\n", message, mp_tag_str(code), (int)code);

    return false;
}

static void ca_fill_asbd_raw(AudioStreamBasicDescription *asbd, int mp_format,
                             int samplerate, int num_channels)
{
    asbd->mSampleRate       = samplerate;
    // Set "AC3" for other spdif formats too - unknown if that works.
    asbd->mFormatID         = af_fmt_is_spdif(mp_format) ?
                              kAudioFormat60958AC3 :
                              kAudioFormatLinearPCM;
    asbd->mChannelsPerFrame = num_channels;
    asbd->mBitsPerChannel   = af_fmt_to_bytes(mp_format) * 8;
    asbd->mFormatFlags      = kAudioFormatFlagIsPacked;
    asbd->mReserved         = 0;

    int channels_per_buffer = num_channels;
    if (af_fmt_is_planar(mp_format)) {
        asbd->mFormatFlags |= kAudioFormatFlagIsNonInterleaved;
        channels_per_buffer = 1;
    }

    if (af_fmt_is_float(mp_format)) {
        asbd->mFormatFlags |= kAudioFormatFlagIsFloat;
    } else if (!af_fmt_is_unsigned(mp_format)) {
        asbd->mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    }

    if (BYTE_ORDER == BIG_ENDIAN)
        asbd->mFormatFlags |= kAudioFormatFlagIsBigEndian;

    asbd->mFramesPerPacket = 1;
    if (asbd->mBitsPerChannel == 32){
       asbd->mBytesPerPacket = asbd->mBytesPerFrame = 6; 
    }else{
    asbd->mBytesPerPacket = asbd->mBytesPerFrame =
        asbd->mFramesPerPacket * channels_per_buffer *
        (asbd->mBitsPerChannel / 8);}
}

void ca_fill_asbd(struct ao *ao, AudioStreamBasicDescription *asbd)
{
    ca_fill_asbd_raw(asbd, ao->format, ao->samplerate, ao->channels.num);
}

bool ca_formatid_is_compressed(uint32_t formatid)
{
    switch (formatid)
    case 'IAC3':
    case 'iac3':
    case  kAudioFormat60958AC3:
    case  kAudioFormatAC3:
        return true;
    return false;
}

// This might be wrong, but for now it's sufficient for us.
static uint32_t ca_normalize_formatid(uint32_t formatID)
{
    return ca_formatid_is_compressed(formatID) ? kAudioFormat60958AC3 : formatID;
}

bool ca_asbd_equals(const AudioStreamBasicDescription *a,
                    const AudioStreamBasicDescription *b)
{
    bool spdif = ca_formatid_is_compressed(a->mFormatID) &&
                 ca_formatid_is_compressed(b->mFormatID);

    int flags = kAudioFormatFlagIsFloat |
    kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsBigEndian;

    return (a->mFormatFlags & flags) == (b->mFormatFlags & flags) &&
          a->mBitsPerChannel == b->mBitsPerChannel &&
          ca_normalize_formatid(a->mFormatID) ==
          ca_normalize_formatid(b->mFormatID) &&
          (spdif || a->mBytesPerPacket == b->mBytesPerPacket) &&
          (spdif || a->mChannelsPerFrame == b->mChannelsPerFrame) &&
          a->mSampleRate == b->mSampleRate;
}

bool ca_asbd_equals_integer_mode_hack(const AudioStreamBasicDescription *a,
                    const AudioStreamBasicDescription *b)
{
    bool spdif = ca_formatid_is_compressed(a->mFormatID) &&
                 ca_formatid_is_compressed(b->mFormatID);
    int flags = kAudioFormatFlagIsFloat |
    kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsBigEndian;

    return (a->mFormatFlags & flags) == (b->mFormatFlags & flags) &&
          a->mBitsPerChannel >= b->mBitsPerChannel &&
          ca_normalize_formatid(a->mFormatID) ==
          ca_normalize_formatid(b->mFormatID) &&
          (spdif || a->mBytesPerPacket == b->mBytesPerPacket) &&
          (spdif || a->mChannelsPerFrame == b->mChannelsPerFrame) &&
          a->mSampleRate == b->mSampleRate;
}


// Return the AF_FORMAT_* (AF_FORMAT_S16 etc.) corresponding to the asbd.
int ca_asbd_to_mp_format(const AudioStreamBasicDescription *asbd)
{
    for (int fmt = 1; fmt < AF_FORMAT_COUNT; fmt++) {
        AudioStreamBasicDescription mp_asbd = {0};
        ca_fill_asbd_raw(&mp_asbd, fmt, asbd->mSampleRate, asbd->mChannelsPerFrame);

        if (ca_asbd_equals(&mp_asbd, asbd))
            return af_fmt_is_spdif(fmt) ? AF_FORMAT_S_AC3 : fmt;
    }
    return 0;
}


int ca_asbd_to_mp_format_integer_mode_hack(const AudioStreamBasicDescription *asbd)
{
    for (int fmt = 1; fmt < AF_FORMAT_COUNT; fmt++) {
        AudioStreamBasicDescription mp_asbd = {0};
        ca_fill_asbd_raw(&mp_asbd, fmt, asbd->mSampleRate, asbd->mChannelsPerFrame);

        if (ca_asbd_equals_integer_mode_hack(&mp_asbd, asbd))
            return af_fmt_is_spdif(fmt) ? AF_FORMAT_S_AC3 : fmt;
    }
    return 0;
}

void ca_print_asbd(struct ao *ao, const char *description,
                   const AudioStreamBasicDescription *asbd)
{
    uint32_t flags  = asbd->mFormatFlags;
    uint32_t swap   = CFSwapInt32HostToBig(asbd->mFormatID);
    char *format    = mp_tag_str(swap);
    int mpfmt       = ca_asbd_to_mp_format_integer_mode_hack(asbd);
    float SampleRate = asbd->mSampleRate / 1000;
    MP_VERBOSE(ao,
       "%s%s %" PRIu32 "/%g/%" PRIu32 "ch "
       "[%" PRIu32 "bpp][%" PRIu32 "fbp]"
       "[%" PRIu32 "bpf] [%" PRIu32 "] "
       "%s%s%s%s%s%s(%s)\n",
       description, format, asbd->mBitsPerChannel, SampleRate,
       asbd->mChannelsPerFrame, asbd->mBytesPerPacket, asbd->mFramesPerPacket,
       asbd->mBytesPerFrame, asbd->mFormatFlags, 
       (flags & kAudioFormatFlagIsFloat) ? "F " : "",
       (flags & kAudioFormatFlagIsPacked) ? "P " : "",
       (flags & kAudioFormatFlagIsSignedInteger) ? "Int " : "",
       (flags & kAudioFormatFlagIsNonMixable) ? "Unmix " : "Mix ",  // "Unmixable" indicates integer mode.
       (flags & kAudioFormatFlagIsAlignedHigh) ? "Aligned High " : "",
       (flags & kAudioFormatFlagIsNonInterleaved) ? "Planar " : "",
       mpfmt ? af_fmt_to_str(mpfmt) : "-");
}

// Return whether new is an improvement over old. Assume a higher value means
// better quality, and we always prefer the value closest to the requested one,
// which is still larger than the requested one.
// Equal values prefer the new one (so ca_asbd_is_better() checks other params).
static bool value_is_better(double req, double old, double new)
{
    if (new >= req) {
        return old < req || new <= old;
    } else {
        return old < req && new >= old;
    }
}

// Return whether new is an improvement over old (req is the requested format).
bool ca_asbd_is_better(AudioStreamBasicDescription *req,
                       AudioStreamBasicDescription *old,
                       AudioStreamBasicDescription *new)
{
    if (new->mChannelsPerFrame > MP_NUM_CHANNELS)
        return false;
    if (old->mChannelsPerFrame > MP_NUM_CHANNELS)
        return true;
    if (req->mFormatID != new->mFormatID)
        return false;
    if (req->mFormatID != old->mFormatID)
        return true;

    if (!value_is_better(req->mBitsPerChannel, old->mBitsPerChannel,
                         new->mBitsPerChannel))
        return false;

    if (!value_is_better(req->mSampleRate, old->mSampleRate, new->mSampleRate))
        return false;

    if (!value_is_better(req->mChannelsPerFrame, old->mChannelsPerFrame,
                         new->mChannelsPerFrame))
        return false;

    return true;
}


bool ca_virtual_asbd_is_better(AudioStreamBasicDescription *req,
                       AudioStreamBasicDescription *old,
                       AudioStreamBasicDescription *new)
{
    if (new->mChannelsPerFrame > MP_NUM_CHANNELS)
        return false;
    if (old->mChannelsPerFrame > MP_NUM_CHANNELS)
        return true;
    if (req->mFormatID != new->mFormatID)
        return false;
    if (req->mFormatID != old->mFormatID)
        return true;

    if (!value_is_better(req->mBitsPerChannel, old->mBitsPerChannel,
                         new->mBitsPerChannel))
        return false;

    if (!value_is_better(req->mSampleRate, old->mSampleRate, new->mSampleRate))
        return false;

    if (!value_is_better(req->mChannelsPerFrame, old->mChannelsPerFrame,
                         new->mChannelsPerFrame))
        return false;

// Optimisation. 
// Turn off Integer Mode. 
// Since "our format" is mixable, check mFormatFlags kAudioFormatFlagIsNonMixable will ensure physical format is also mixable.
// In my system, W/O comparing mFormatFlags will make mpv choose unmixable format.
// Now, virtual format will automatically set to 32-bit mixable float point and 
// bypass the "24-bit padded in 32-bit" issue.
   
    if ((req->mFormatFlags & kAudioFormatFlagIsNonMixable) != (new->mFormatFlags & kAudioFormatFlagIsNonMixable))
        return false;
    if ((req->mFormatFlags & kAudioFormatFlagIsNonMixable) != (old->mFormatFlags & kAudioFormatFlagIsNonMixable))
        return true;  
           
    return true;
}

// Optimisation (maybe). 
// No. of channels should have higher priority than Bit depth when connecting to bluetooth.
// My bluetooth device only have 2 physical formats. "16/8 1ch" and "32/48 2ch". 
// The virtual format of my bluetooth device is "32-bit float" only. 
// Not comparing mBitsPerChannel will lead to a "32-bit float" physical format even if a 16-bit file is played.
// It doesn't really matter as long as CoreAudio can handle it.

bool ca_bluetooth_asbd_is_better(AudioStreamBasicDescription *req,
                       AudioStreamBasicDescription *old,
                       AudioStreamBasicDescription *new)
{
    if (new->mChannelsPerFrame > MP_NUM_CHANNELS)
        return false;
    if (old->mChannelsPerFrame > MP_NUM_CHANNELS)
        return true;
    if (req->mFormatID != new->mFormatID)
        return false;
    if (req->mFormatID != old->mFormatID)
        return true;

    if (!value_is_better(5, old->mBytesPerFrame,
                         new->mBytesPerFrame))
        return false;

    if (!value_is_better(req->mSampleRate, old->mSampleRate, new->mSampleRate))
        return false;

    if (!value_is_better(req->mChannelsPerFrame, old->mChannelsPerFrame,
                         new->mChannelsPerFrame))
        return false; 

    return true;
}

int64_t ca_frames_to_us(struct ao *ao, uint32_t frames)
{
    return frames / (float) ao->samplerate * 1e6;
}

int64_t ca_get_latency(const AudioTimeStamp *ts)
{
#if HAVE_COREAUDIO
    uint64_t out = AudioConvertHostTimeToNanos(ts->mHostTime);
    uint64_t now = AudioConvertHostTimeToNanos(AudioGetCurrentHostTime());

    if (now > out)
        return 0;

    return (out - now) * 1e-3;
#else
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0)
        mach_timebase_info(&timebase);

    uint64_t out = ts->mHostTime;
    uint64_t now = mach_absolute_time();

    if (now > out)
        return 0;

    return (out - now) * timebase.numer / timebase.denom / 1e3;
#endif
}

#if HAVE_COREAUDIO
bool ca_stream_supports_compressed(struct ao *ao, AudioStreamID stream)
{
    AudioStreamRangedDescription *formats = NULL;
    size_t n_formats;

    OSStatus err =
        CA_GET_ARY(stream, kAudioStreamPropertyAvailablePhysicalFormats,
                   &formats, &n_formats);

    CHECK_CA_ERROR("Could not get number of stream formats.");

    for (int i = 0; i < n_formats; i++) {
        AudioStreamBasicDescription asbd = formats[i].mFormat;

        ca_print_asbd(ao, "- ", &asbd);

        if (ca_formatid_is_compressed(asbd.mFormatID)) {
            talloc_free(formats);
            return true;
        }
    }

    talloc_free(formats);
coreaudio_error:
    return false;
}

OSStatus ca_lock_device(AudioDeviceID device, pid_t *pid)
{
    *pid = getpid();
    OSStatus err = CA_SET(device, kAudioDevicePropertyHogMode, pid);
    if (err != noErr)
        *pid = -1;

    return err;
}

OSStatus ca_unlock_device(AudioDeviceID device, pid_t *pid)
{
    if (*pid == getpid()) {
        *pid = -1;
        return CA_SET(device, kAudioDevicePropertyHogMode, &pid);
    }
    return noErr;
}

static OSStatus ca_change_mixing(struct ao *ao, AudioDeviceID device,
                                 uint32_t val, bool *changed)
{
    *changed = false;

    AudioObjectPropertyAddress p_addr = (AudioObjectPropertyAddress) {
        .mSelector = kAudioDevicePropertySupportsMixing,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMaster,
    };

    if (AudioObjectHasProperty(device, &p_addr)) {
        OSStatus err;
        Boolean writeable = 0;
        err = CA_SETTABLE(device, kAudioDevicePropertySupportsMixing,
                          &writeable);

        if (!CHECK_CA_WARN("can't tell if mixing property is settable")) {
            return err;
        }

        if (!writeable)
            return noErr;

        err = CA_SET(device, kAudioDevicePropertySupportsMixing, &val);
        if (err != noErr)
            return err;

        if (!CHECK_CA_WARN("can't set mix mode")) {
            return err;
        }

        *changed = true;
    }

    return noErr;
}

OSStatus ca_disable_mixing(struct ao *ao, AudioDeviceID device, bool *changed)
{
    return ca_change_mixing(ao, device, 0, changed);
}

OSStatus ca_enable_mixing(struct ao *ao, AudioDeviceID device, bool changed)
{
    if (changed) {
        bool dont_care = false;
        return ca_change_mixing(ao, device, 1, &dont_care);
    }

    return noErr;
}

int64_t ca_get_device_latency_us(struct ao *ao, AudioDeviceID device)
{
    uint32_t latency_frames = 0;
    uint32_t latency_properties[] = {
        kAudioDevicePropertyLatency,
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertySafetyOffset,
    };
    for (int n = 0; n < MP_ARRAY_SIZE(latency_properties); n++) {
        uint32_t temp;
        OSStatus err = CA_GET_O(device, latency_properties[n], &temp);
        CHECK_CA_WARN("cannot get device latency");
        if (err == noErr) {
            latency_frames += temp;
            MP_VERBOSE(ao, "Latency property %s: %d frames\n",
                       mp_tag_str(CFSwapInt32HostToBig(latency_properties[n])),
                        (int)temp);
        }
    }

    return ca_frames_to_us(ao, latency_frames);
}

static OSStatus ca_change_format_listener(
    AudioObjectID object, uint32_t n_addresses,
    const AudioObjectPropertyAddress addresses[],
    void *data)
{
    sem_t *sem = data;
    sem_post(sem);
    return noErr;
}

bool ca_change_physical_format_sync(struct ao *ao, AudioStreamID stream,
                                    AudioStreamBasicDescription change_format)
{
    OSStatus err = noErr;
    bool format_set = false;

    ca_print_asbd(ao, "Setting stream physical format:", &change_format);

    sem_t wakeup;
    if (sem_init(&wakeup, 0, 0)) {
        MP_WARN(ao, "OOM\n");
        return false;
    }

    AudioStreamBasicDescription prev_format;
    err = CA_GET(stream, kAudioStreamPropertyPhysicalFormat, &prev_format);
    CHECK_CA_ERROR("can't get current physical format");

    ca_print_asbd(ao, "Format in use before switching:", &prev_format);

    /* Install the callback. */
    AudioObjectPropertyAddress p_addr = {
        .mSelector = kAudioStreamPropertyPhysicalFormat,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMaster,
    };

    err = AudioObjectAddPropertyListener(stream, &p_addr,
                                         ca_change_format_listener,
                                         &wakeup);
    CHECK_CA_ERROR("can't add property listener during format change");

    /* Change the format. */
    err = CA_SET(stream, kAudioStreamPropertyPhysicalFormat, &change_format);
    CHECK_CA_WARN("error changing physical format");

    /* The AudioStreamSetProperty is not only asynchronous,
     * it is also not Atomic, in its behaviour. */
    struct timespec timeout = mp_rel_time_to_timespec(2.0);
    AudioStreamBasicDescription actual_format = {0};
    while (1) {
        err = CA_GET(stream, kAudioStreamPropertyPhysicalFormat, &actual_format);
        if (!CHECK_CA_WARN("could not retrieve physical format"))
            break;

        format_set = ca_asbd_equals(&change_format, &actual_format);
        if (format_set)
            break;

        if (sem_timedwait(&wakeup, &timeout)) {
            MP_VERBOSE(ao, "reached timeout\n");
            break;
        }
    }

    ca_print_asbd(ao, "Actual format in use:", &actual_format);

    if (!format_set) {
        MP_WARN(ao, "changing physical format failed\n");
        // Some drivers just fuck up and get into a broken state. Restore the
        // old format in this case.
        err = CA_SET(stream, kAudioStreamPropertyPhysicalFormat, &prev_format);
        CHECK_CA_WARN("error restoring physical format");
    }

    err = AudioObjectRemovePropertyListener(stream, &p_addr,
                                            ca_change_format_listener,
                                            &wakeup);
    CHECK_CA_ERROR("can't remove property listener");

coreaudio_error:
    sem_destroy(&wakeup);
    return format_set;
}
#endif
