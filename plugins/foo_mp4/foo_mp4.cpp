/*
** FAAD - Freeware Advanced Audio Decoder
** Copyright (C) 2002-2003 M. Bakker
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** $Id: foo_mp4.cpp,v 1.17 2003/04/02 21:11:56 menno Exp $
**/

#include <mp4.h>
#include <faad.h>
#include "pfc/pfc.h"
#include "foobar2000/SDK/input.h"
#include "foobar2000/SDK/console.h"
#include "foobar2000/SDK/componentversion.h"

char *STRIP_REVISION(const char *str)
{
    char *tmp = strchr(str, ' ');
    tmp[strlen(tmp)-2] = '\0';
    return tmp;
}

DECLARE_COMPONENT_VERSION ("MPEG-4 AAC decoder",
                           STRIP_REVISION("$Revision: 1.17 $"),
                           "Based on FAAD2 v" FAAD2_VERSION "\nhttp://www.audiocoding.com\n""\n""Copyright (C) 2002-2003 M. Bakker" );

class input_mp4 : public input
{
public:

    virtual int test_filename(const char * fn,const char * ext)
    {
        return !stricmp(ext,"MP4");
    }

    virtual int open(reader *r, file_info *info, int full_open)
    {
        unsigned __int8 *buffer;
        unsigned __int32 buffer_size;
        unsigned __int8 channels;
        unsigned __int32 samplerate;
        faacDecConfigurationPtr config;

        m_reader = r;

        hDecoder = faacDecOpen();
        if (!hDecoder)
        {
            console::error("Failed to open FAAD2 library.", "foo_mp4");
            return 0;
        }

        config = faacDecGetCurrentConfiguration(hDecoder);
        config->outputFormat = FAAD_FMT_DOUBLE;
        faacDecSetConfiguration(hDecoder, config);

        hFile = MP4ReadCb(0, open_cb, close_cb, read_cb, write_cb,
            setpos_cb, getpos_cb, filesize_cb, (void*)m_reader);
        if (hFile == MP4_INVALID_FILE_HANDLE)
        {
            console::error("Failed to open MP4 file.", "foo_mp4");
            return 0;
        }

        track = GetAACTrack(hFile);
        if (track < 1)
        {
            console::error("No valid AAC track found.", "foo_mp4");
            return 0;
        }

        buffer = NULL;
        buffer_size = 0;
        MP4GetTrackESConfiguration(hFile, track, &buffer, &buffer_size);
        if (!buffer)
        {
            console::error("Unable to read track specific configuration.", "foo_mp4");
            return 0;
        }

        int rc = faacDecInit2(hDecoder, (unsigned char*)buffer, buffer_size,
            (unsigned long*)&samplerate, (unsigned char*)&channels);
        if (buffer) free(buffer);
        if (rc < 0)
        {
            console::error("Unable to initialise FAAD2 library.", "foo_mp4");
            return 0;
        }

        numSamples = MP4GetTrackNumberOfSamples(hFile, track);
        sampleId = 1;

        unsigned __int64 length = MP4GetTrackDuration(hFile, track);
        __int64 msDuration = MP4ConvertFromTrackDuration(hFile, track,
            length, MP4_MSECS_TIME_SCALE);
        info->set_length((double)msDuration/1000.0);

        info->info_set_int("bitrate",(__int64)(1.0/1000.0 *
            (double)(__int64)MP4GetTrackIntegerProperty(hFile,
            track, "mdia.minf.stbl.stsd.mp4a.esds.decConfigDescr.avgBitrate")) + 0.5);
        info->info_set_int("channels", (__int64)channels);
        info->info_set_int("samplerate", (__int64)samplerate);

        ReadMP4Tag(info);

        return 1;
    }

    input_mp4()
    {
        hFile = MP4_INVALID_FILE_HANDLE;
        hDecoder = NULL;
    }

    ~input_mp4()
    {
        if (hFile != MP4_INVALID_FILE_HANDLE)
            MP4Close(hFile);
        if (hDecoder)
            faacDecClose(hDecoder);
    }

    virtual int run(audio_chunk * chunk)
    {
        faacDecFrameInfo frameInfo;
        unsigned char *buffer;
        unsigned __int32 buffer_size;
        void *sample_buffer;

        if (sampleId == MP4_INVALID_SAMPLE_ID)
        {
            console::error("Invalid sampleId.", "foo_mp4");
            return 0;
        }

        do {
            buffer = NULL;
            buffer_size = 0;

            MP4ReadSample(hFile, track, sampleId,
                (unsigned __int8**)&buffer, &buffer_size,
                NULL, NULL, NULL, NULL);
            sampleId++;

            sample_buffer = faacDecDecode(hDecoder, &frameInfo, buffer, buffer_size);

            if (buffer) free(buffer);

        } while ((frameInfo.error == 0) && (frameInfo.samples == 0));

        if (frameInfo.error || (sampleId > numSamples))
        {
            if (frameInfo.error)
                console::error(faacDecGetErrorMessage(frameInfo.error), "foo_mp4");
            return 0;
        }

        chunk->data = (audio_sample*)sample_buffer;
        chunk->samples = frameInfo.samples/frameInfo.channels;
        chunk->nch = frameInfo.channels;
        chunk->srate = frameInfo.samplerate;

        return 1;
    }

    virtual int set_info(reader *r,const file_info * info)
    {
        m_reader = r;

        hFile = MP4ModifyCb(0, 0, open_cb, close_cb, read_cb, write_cb,
            setpos_cb, getpos_cb, filesize_cb, (void*)m_reader);
        if (hFile == MP4_INVALID_FILE_HANDLE) return 0;

        track = GetAACTrack(hFile);
        if (track < 1)
        {
            console::error("No valid AAC track found.", "foo_mp4");
            return 0;
        }

        MP4TagDelete(hFile, track);

        /* replay gain writing */
        const char *p = NULL;

        p = info->info_get("REPLAYGAIN_TRACK_PEAK");
        if (p)
            MP4TagAddEntry(hFile, track, "REPLAYGAIN_TRACK_PEAK", p);
        p = info->info_get("REPLAYGAIN_TRACK_GAIN");
        if (p)
            MP4TagAddEntry(hFile, track, "REPLAYGAIN_TRACK_GAIN", p);
        p = info->info_get("REPLAYGAIN_ALBUM_PEAK");
        if (p)
            MP4TagAddEntry(hFile, track, "REPLAYGAIN_ALBUM_PEAK", p);
        p = info->info_get("REPLAYGAIN_ALBUM_GAIN");
        if (p)
            MP4TagAddEntry(hFile, track, "REPLAYGAIN_ALBUM_GAIN", p);

        int numItems = info->meta_get_count();
        if (numItems > 0)
        {
            for (int i = 0; i < numItems; i++)
            {
                const char *n = info->meta_enum_name(i);
                const char *v = info->meta_enum_value(i);
                MP4TagAddEntry(hFile, track, n, v);
            }
        }

        numItems = MP4TagGetNumEntries(hFile, track);
        if (numItems == 0)
            MP4TagDelete(hFile, track);

        /* end */
        return 1;
    }

    virtual int seek(double seconds)
    {
        MP4Duration duration;

        duration = MP4ConvertToTrackDuration(hFile,
            track, seconds, MP4_SECS_TIME_SCALE);
        sampleId = MP4GetSampleIdFromTime(hFile,
            track, duration, 0);

        return 1;
    }
    
    virtual int is_our_content_type(const char *url, const char *type)
    {
        return !strcmp(type, "audio/mp4") || !strcmp(type, "audio/x-mp4");
    }

private:

    reader *m_reader;

    MP4FileHandle hFile;
    MP4SampleId sampleId, numSamples;
    MP4TrackId track;

    faacDecHandle hDecoder;

    int ReadMP4Tag(file_info *info)
    {
        int numItems = MP4TagGetNumEntries(hFile, track);

        for (int i = 0; i < numItems; i++)
        {
            float f = 0.0;
            const char *n = NULL, *v = NULL;

            MP4TagGetEntry(hFile, track, i, &n, &v);

            if (!strcmp(n, "REPLAYGAIN_TRACK_PEAK"))
            {
                sscanf(v, "%f", &f);
                info->info_set_replaygain_track_peak((double)f);
            } else if (!strcmp(n, "REPLAYGAIN_TRACK_GAIN")) {
                sscanf(v, "%f", &f);
                info->info_set_replaygain_track_gain((double)f);
            } else if (!strcmp(n, "REPLAYGAIN_ALBUM_PEAK")) {
                sscanf(v, "%f", &f);
                info->info_set_replaygain_album_peak((double)f);
            } else if (!strcmp(n, "REPLAYGAIN_ALBUM_GAIN")) {
                sscanf(v, "%f", &f);
                info->info_set_replaygain_album_gain((double)f);
            } else {
                info->meta_add(n, v);
            }
        }

        return 1;
    }

    int GetAACTrack(MP4FileHandle infile)
    {
        /* find AAC track */
        int i, rc;
        int numTracks = MP4GetNumberOfTracks(infile, NULL, /* subType */ 0);

        for (i = 0; i < numTracks; i++)
        {
            MP4TrackId trackId = MP4FindTrackId(infile, i, NULL, /* subType */ 0);
            const char* trackType = MP4GetTrackType(infile, trackId);

            if (!strcmp(trackType, MP4_AUDIO_TRACK_TYPE))
            {
                unsigned char *buff = NULL;
                int buff_size = 0;
                mp4AudioSpecificConfig mp4ASC;

                MP4GetTrackESConfiguration(infile, trackId,
                    (unsigned __int8**)&buff, (unsigned __int32*)&buff_size);

                if (buff)
                {
                    rc = AudioSpecificConfig(buff, buff_size, &mp4ASC);
                    free(buff);

                    if (rc < 0)
                        return -1;
                    return trackId;
                }
            }
        }

        /* can't decode this */
        return -1;
    }

    /* file callback stuff */
    static unsigned __int32 open_cb(const char *pName,
        const char *mode, void *userData)
    {
        return 1;
    }

    static void close_cb(void *userData)
    {
        return;
    }

    static unsigned __int32 read_cb(void *pBuffer, unsigned int nBytesToRead,
        void *userData)
    {
        reader *r = (reader*)userData;
        return r->read(pBuffer, nBytesToRead);
    }

    static unsigned __int32 write_cb(void *pBuffer, unsigned int nBytesToWrite,
        void *userData)
    {
        reader *r = (reader*)userData;
        return r->write(pBuffer, nBytesToWrite);
    }

    static __int64 getpos_cb(void *userData)
    {
        reader *r = (reader*)userData;
        return r->get_position();
    }

    static __int32 setpos_cb(unsigned __int32 pos, void *userData)
    {
        reader *r = (reader*)userData;
        return !(r->seek(pos));
    }

    static __int64 filesize_cb(void *userData)
    {
        reader *r = (reader*)userData;
        return r->get_length();
    }
};

static service_factory_t<input,input_mp4> foo;
