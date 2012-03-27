/******************************************************************************
* Copyright (c) 2012, Michael P. Gerlek (mpg@flaxen.com)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Consulting LLC nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include "NitfFile.hpp"
#ifdef PDAL_HAVE_GDAL

#include "cpl_string.h"

#include <pdal/Metadata.hpp>


namespace pdal { namespace drivers { namespace nitf {

static const std::string s_namespace("drivers.nitf.reader");


// this is a copy of the GDAL function, because the GDAL function isn't exported
static char* myNITFGetField(char *pszTarget, const char *pszSource, 
                            int nStart, int nLength)
{
    memcpy( pszTarget, pszSource + nStart, nLength );
    pszTarget[nLength] = '\0';

    return pszTarget;
}


// --------------------------------------------------------------------------


NitfFile::NitfFile(const std::string& filename)
    : m_filename(filename)
    , m_file(NULL)
    , m_imageSegmentNumber(0)
    , m_lidarSegmentNumber(0)
{
    return;
}


NitfFile::~NitfFile()
{
    close();
    return;
}


void NitfFile::open()
{
    m_file = NITFOpen(m_filename.c_str(), FALSE);
    if (!m_file)
    {
        throw pdal_error("unable to open NITF file");
    }
    
    m_imageSegmentNumber = findIMSegment();
    m_lidarSegmentNumber = findLIDARASegment();

    return;
}


void NitfFile::close()
{
    if (m_file)
    {
        NITFClose(m_file);
        m_file = NULL;
    }

    return;
}


void NitfFile::getLasPosition(boost::uint64_t& offset, boost::uint64_t& length) const
{
    NITFDES* dataSegment = NITFDESAccess(m_file, m_lidarSegmentNumber);
    if (!dataSegment)
    {
        throw pdal_error("NITFDESAccess failed");
    }

    // grab the file offset info
    NITFSegmentInfo* psSegInfo = dataSegment->psFile->pasSegmentInfo + dataSegment->iSegment;
    offset = psSegInfo->nSegmentStart;
    length = psSegInfo->nSegmentSize;

    NITFDESDeaccess(dataSegment);

    return;
}


void NitfFile::extractMetadata(std::vector<Metadata>& metadatums)
{
    //
    // file header fields and TREs
    //
    {
        processMetadata(m_file->papszMetadata, metadatums, "FH");

        processTREs(m_file->nTREBytes, m_file->pachTRE, metadatums, "FH");
    }

    //
    // IM segment fields
    //
    {
        NITFImage* imageSegment = NITFImageAccess(m_file, m_imageSegmentNumber);
        if (!imageSegment)
        {
            throw pdal_error("NITFImageAccess failed");
        }

        processMetadata(imageSegment->papszMetadata, metadatums, "IM");

        processTREs(imageSegment->nTREBytes, imageSegment->pachTRE, metadatums, "IM");

        NITFImageDeaccess(imageSegment);
    }

    //
    // LIDARA segment fields
    //
    {
        NITFDES* dataSegment = NITFDESAccess(m_file, m_lidarSegmentNumber);
        if (!dataSegment)
        {
            throw pdal_error("NITFDESAccess failed");
        }

        processMetadata(dataSegment->papszMetadata, metadatums, "DES");

        processTREs_DES(dataSegment, metadatums, "IM");

        NITFDESDeaccess(dataSegment);
    }

    return;
}


//---------------------------------------------------------------------------


// return the IID1 field as a string
std::string NitfFile::getSegmentIdentifier(NITFSegmentInfo* psSegInfo)
{
    vsi_l_offset curr = VSIFTellL(m_file->fp);

    VSIFSeekL(m_file->fp, psSegInfo->nSegmentHeaderStart + 2, SEEK_SET);
    char p[11];
    if (VSIFReadL(p, 1, 10, m_file->fp) != 10)
    {
        throw pdal_error("error reading nitf");
    }
    p[10] = 0;
    std::string s = p;
    
    VSIFSeekL(m_file->fp, curr, SEEK_SET);
    return s;
}


// return DESVER as a string
std::string NitfFile::getDESVER(NITFSegmentInfo* psSegInfo)
{
    vsi_l_offset curr = VSIFTellL(m_file->fp);

    VSIFSeekL(m_file->fp, psSegInfo->nSegmentHeaderStart + 2 + 25, SEEK_SET);
    char p[3];
    if (VSIFReadL(p, 1, 2, m_file->fp) != 2)
    {
        throw pdal_error("error reading nitf");
    }
    p[2] = 0;
    std::string s = p;
    
    VSIFSeekL(m_file->fp, curr, SEEK_SET);
    return s;
}


int NitfFile::findIMSegment()
{
    // as per 3.2.3 (pag 19) and 3.2.4 (page 39)

    int iSegment(0);
    NITFSegmentInfo *psSegInfo = NULL;
    for(iSegment = 0;  iSegment < m_file->nSegmentCount; iSegment++ )
    {
        psSegInfo = m_file->pasSegmentInfo + iSegment;

        if (strncmp(psSegInfo->szSegmentType,"IM",2)==0)
        {
            const std::string iid1 = getSegmentIdentifier(psSegInfo);
            // BUG: shouldn't allow "None" here!
            if (iid1 == "INTENSITY " || iid1 == "ELEVATION " || iid1 == "None      ")
            {
                return iSegment;
            }
        }
    }    

    throw pdal_error("Unable to find Image segment from NITF file");
}


int NitfFile::findLIDARASegment()
{
    // as per 3.2.5, page 59

    int iSegment(0);
    NITFSegmentInfo *psSegInfo = NULL;
    for(iSegment = 0;  iSegment < m_file->nSegmentCount; iSegment++ )
    {
        psSegInfo = m_file->pasSegmentInfo + iSegment;
        if (strncmp(psSegInfo->szSegmentType,"DE",2)==0)
        {
            const std::string iid1 = getSegmentIdentifier(psSegInfo);
            const std::string desver = getDESVER(psSegInfo);
            if (iid1 == "LIDARA DES" && desver == "01")
            {
                return iSegment;
            }
        }
    }    

    throw pdal_error("Unable to find LIDARA data extension segment from NITF file");
}


void NitfFile::processTREs(int nTREBytes, const char *pszTREData, std::vector<Metadata>& metadatums, const std::string& parentkey)
{
    char* szTemp = new char[nTREBytes];

    while( nTREBytes > 10 )
    {
        int nThisTRESize = atoi(myNITFGetField(szTemp, pszTREData, 6, 5 ));
        if (nThisTRESize < 0 || nThisTRESize > nTREBytes - 11)
        {
            break;
        }

        char key[7];
        strncpy(key, pszTREData, 6);
        key[6] = 0;

        Metadata m(key, s_namespace + "." + parentkey);

        const std::string value(pszTREData + 11);
        //std::vector<boost::uint8_t> data(nThisTRESize);
        //boost::uint8_t* p = (boost::uint8_t*)(pszTREData + 11);
        //for (int i=0; i<nThisTRESize; i++) data[i] = p[i];
        //ByteArray value(data);
        m.setValue<std::string>(value);

        metadatums.push_back(m);

        pszTREData += nThisTRESize + 11;
        nTREBytes -= (nThisTRESize + 11);
    }

    delete[] szTemp;

    return;
}


void NitfFile::processTREs_DES(NITFDES* dataSegment, std::vector<Metadata>& metadatums, const std::string& parentkey)
{
    char* pabyTREData = NULL;
    int nOffset = 0;
    char szTREName[7];
    int nThisTRESize;

    while (NITFDESGetTRE( dataSegment, nOffset, szTREName, &pabyTREData, &nThisTRESize))
    {
        char key[7];
        strncpy(key, pabyTREData, 6);
        key[6] = 0;

        const std::string value(pabyTREData + 11);

        Metadata m(key, s_namespace + "." + parentkey);
        m.setValue<std::string>(value);
        metadatums.push_back(m);

        nOffset += 11 + nThisTRESize;

        NITFDESFreeTREData(pabyTREData);
    }

    return;
}


void NitfFile::processMetadata(char** papszMetadata, std::vector<Metadata>& metadatums, const std::string& parentkey)
{
    int cnt = CSLCount(papszMetadata);
    for (int i=0; i<cnt; i++)
    {
        const char* p = papszMetadata[i];
        const std::string s(p);

        const int sep = s.find('=');
        const std::string key = s.substr(5, sep-5);
        const std::string value = s.substr(sep+1, std::string::npos);

        Metadata m(key, s_namespace + "." + parentkey);
        m.setValue<std::string>(value);
        metadatums.push_back(m);
    }

    return;
}

} } } // namespaces
#endif