/******************************************************************************
 *
 * Purpose:  Implementation of the CPCIDSK_LUT class.
 *
 ******************************************************************************
 * Copyright (c) 2015
 * PCI Geomatics, 90 Allstate Parkway, Markham, Ontario, Canada.
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "pcidsk_exception.h"
#include "segment/cpcidsklut.h"
#include <cassert>
#include <cstring>

using namespace PCIDSK;

PCIDSK_LUT::~PCIDSK_LUT() = default;

/************************************************************************/
/*                            CPCIDSK_LUT()                             */
/************************************************************************/

CPCIDSK_LUT::CPCIDSK_LUT( PCIDSKFile *fileIn, int segmentIn,
                          const char *segment_pointer )
        : CPCIDSKSegment( fileIn, segmentIn, segment_pointer )

{
}

/************************************************************************/
/*                           ~CPCIDSKGeoref()                           */
/************************************************************************/

CPCIDSK_LUT::~CPCIDSK_LUT()

{
}

/************************************************************************/
/*                              ReadLUT()                               */
/************************************************************************/

void CPCIDSK_LUT::ReadLUT(std::vector<unsigned char>& lut)

{
    PCIDSKBuffer seg_data;

    seg_data.SetSize(256*4);

    ReadFromFile( seg_data.buffer, 0, 256*4);

    lut.resize(256);
    for( int i = 0; i < 256; i++ )
    {
        lut[i] = (unsigned char) seg_data.GetInt(0+i*4, 4);
    }
}

/************************************************************************/
/*                              WriteLUT()                              */
/************************************************************************/

void CPCIDSK_LUT::WriteLUT(const std::vector<unsigned char>& lut)

{
    if(lut.size() != 256)
    {
        throw PCIDSKException("LUT must contain 256 entries (%d given)", static_cast<int>(lut.size()));
    }

    PCIDSKBuffer seg_data;

    seg_data.SetSize(256*4);

    ReadFromFile( seg_data.buffer, 0, 256*4 );

    int i;
    for( i = 0; i < 256; i++ )
    {
        seg_data.Put( (int) lut[i], i*4, 4);
    }

    WriteToFile( seg_data.buffer, 0, 256*4 );
}
