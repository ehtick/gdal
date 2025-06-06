/******************************************************************************
 *
 * Purpose:  Declaration of the PCIDSKChannel interface.
 *
 ******************************************************************************
 * Copyright (c) 2009
 * PCI Geomatics, 90 Allstate Parkway, Markham, Ontario, Canada.
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/
#ifndef INCLUDE_PCIDSK_CHANNEL_H
#define INCLUDE_PCIDSK_CHANNEL_H

#include "pcidsk_types.h"
#include <string>
#include <vector>

namespace PCIDSK
{
/************************************************************************/
/*                            PCIDSKChannel                             */
/************************************************************************/

//! Interface to one PCIDSK channel (band) or bitmap segment.

    class PCIDSK_DLL PCIDSKChannel
    {
    public:
        virtual ~PCIDSKChannel();
        virtual int GetBlockWidth() const = 0;
        virtual int GetBlockHeight() const = 0;
        virtual int GetBlockCount() const = 0;
        virtual int GetWidth() const = 0;
        virtual int GetHeight() const = 0;
        virtual eChanType GetType() const = 0;
        virtual int ReadBlock( int block_index, void *buffer,
            int win_xoff=-1, int win_yoff=-1,
            int win_xsize=-1, int win_ysize=-1 ) = 0;
        virtual int WriteBlock( int block_index, void *buffer ) = 0;
        virtual int GetOverviewCount() = 0;
        virtual PCIDSKChannel *GetOverview( int i ) = 0;
        virtual bool IsOverviewValid( int i ) = 0;
        virtual std::string GetOverviewResampling( int i ) = 0;
        virtual void SetOverviewValidity( int i, bool validity ) = 0;
        virtual std::vector<int> GetOverviewLevelMapping() const = 0;

        virtual std::string GetMetadataValue( const std::string &key ) const = 0;
        virtual void SetMetadataValue( const std::string &key, const std::string &value ) = 0;
        virtual std::vector<std::string> GetMetadataKeys() const = 0;

        virtual void Synchronize() = 0;

        virtual std::string GetDescription() = 0;
        virtual void SetDescription( const std::string &description ) = 0;

        virtual std::vector<std::string> GetHistoryEntries() const = 0;
        virtual void SetHistoryEntries( const std::vector<std::string> &entries ) = 0;
        virtual void PushHistory(const std::string &app,
                                 const std::string &message) = 0;

        // Only applicable to FILE interleaved raw channels.
        virtual void GetChanInfo( std::string &filename, uint64 &image_offset,
                                  uint64 &pixel_offset, uint64 &line_offset,
                                  bool &little_endian ) const = 0;
        virtual void SetChanInfo( std::string filename, uint64 image_offset,
                                  uint64 pixel_offset, uint64 line_offset,
                                  bool little_endian ) = 0;

        // Only applicable to CExternalChannels
        virtual void GetEChanInfo( std::string &filename, int &echannel,
                                   int &exoff, int &eyoff,
                                   int &exsize, int &eysize ) const = 0;
        virtual void SetEChanInfo( std::string filename, int echannel,
                                   int exoff, int eyoff,
                                   int exsize, int eysize ) = 0;
    };
} // end namespace PCIDSK

#endif // INCLUDE_PCIDSK_CHANNEL_H
