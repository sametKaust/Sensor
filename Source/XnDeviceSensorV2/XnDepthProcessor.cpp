/*****************************************************************************
*                                                                            *
*  PrimeSense Sensor 5.x Alpha                                               *
*  Copyright (C) 2012 PrimeSense Ltd.                                        *
*                                                                            *
*  This file is part of PrimeSense Sensor                                    *
*                                                                            *
*  Licensed under the Apache License, Version 2.0 (the "License");           *
*  you may not use this file except in compliance with the License.          *
*  You may obtain a copy of the License at                                   *
*                                                                            *
*      http://www.apache.org/licenses/LICENSE-2.0                            *
*                                                                            *
*  Unless required by applicable law or agreed to in writing, software       *
*  distributed under the License is distributed on an "AS IS" BASIS,         *
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
*  See the License for the specific language governing permissions and       *
*  limitations under the License.                                            *
*                                                                            *
*****************************************************************************/
//---------------------------------------------------------------------------
// Includes
//---------------------------------------------------------------------------
#include "XnDepthProcessor.h"
#include "XnSensor.h"
#include <XnProfiling.h>
#include <XnLog.h>

//---------------------------------------------------------------------------
// Defines
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Code
//---------------------------------------------------------------------------
XnDepthProcessor::XnDepthProcessor(XnSensorDepthStream* pStream, XnSensorStreamHelper* pHelper, XnFrameBufferManager* pBufferManager) :
	XnFrameStreamProcessor(pStream, pHelper, pBufferManager, XN_SENSOR_PROTOCOL_RESPONSE_DEPTH_START, XN_SENSOR_PROTOCOL_RESPONSE_DEPTH_END),
	m_pShiftToDepthTable(pStream->GetShiftToDepthTable()),
	m_nPaddingPixelsOnEnd(0),
	m_bShiftToDepthAllocated(FALSE),
	m_nExpectedFrameSize(0)
{
}

XnDepthProcessor::~XnDepthProcessor()
{
	if (m_bShiftToDepthAllocated)
	{
		xnOSFree(m_pShiftToDepthTable);
	}
}

XnStatus XnDepthProcessor::Init()
{
	XnStatus nRetVal = XN_STATUS_OK;
	
	// init base
	nRetVal = XnFrameStreamProcessor::Init();
	XN_IS_STATUS_OK(nRetVal);

	switch (GetStream()->GetOutputFormat())
	{
	case XN_OUTPUT_FORMAT_SHIFT_VALUES:
		{
			// optimization. We create a LUT shift-to-shift. See comment up.
			m_pShiftToDepthTable = (XnDepthPixel*)xnOSMalloc(sizeof(XnDepthPixel)*XN_DEVICE_SENSOR_MAX_SHIFT_VALUE);
			XN_VALIDATE_ALLOC_PTR(m_pShiftToDepthTable);
			for (XnUInt32 i = 0; i < XN_DEVICE_SENSOR_MAX_SHIFT_VALUE; ++i)
			{
				m_pShiftToDepthTable[i] = (XnDepthPixel)i;
			}
			m_bShiftToDepthAllocated = TRUE;
		}
		break;
	case XN_OUTPUT_FORMAT_DEPTH_VALUES:
		break;
	default:
		XN_LOG_WARNING_RETURN(XN_STATUS_ERROR, XN_MASK_SENSOR_PROTOCOL_DEPTH, "Unknown Depth output: %d", GetStream()->GetOutputFormat());
	}

	return (XN_STATUS_OK);
}

void XnDepthProcessor::OnStartOfFrame(const XnSensorProtocolResponseHeader* pHeader)
{
	// call base
	XnFrameStreamProcessor::OnStartOfFrame(pHeader);

	m_nExpectedFrameSize = CalculateExpectedSize();

	if (m_pDevicePrivateData->FWInfo.nFWVer >= XN_SENSOR_FW_VER_5_1 && pHeader->nTimeStamp != 0)
	{
		// PATCH: starting with v5.1, the timestamp field of the SOF packet, is the number of pixels
		// that should be prepended to the frame.
		XnUInt32 nPaddingPixelsOnStart = pHeader->nTimeStamp >> 16;
		m_nPaddingPixelsOnEnd = pHeader->nTimeStamp & 0x0000FFFF;

		PadPixels(nPaddingPixelsOnStart);
	}
}

XnUInt32 XnDepthProcessor::CalculateExpectedSize()
{
	XnUInt32 nExpectedDepthBufferSize = GetStream()->GetXRes() * GetStream()->GetYRes();

	// when cropping is turned on, actual depth size is smaller
	if (GetStream()->m_FirmwareCropMode.GetValue() != XN_FIRMWARE_CROPPING_MODE_DISABLED)
	{
		nExpectedDepthBufferSize = (XnUInt32)(GetStream()->m_FirmwareCropSizeX.GetValue() * GetStream()->m_FirmwareCropSizeY.GetValue());
	}

	nExpectedDepthBufferSize *= sizeof(XnDepthPixel);

	return nExpectedDepthBufferSize;
}

void XnDepthProcessor::OnEndOfFrame(const XnSensorProtocolResponseHeader* pHeader)
{
	// pad pixels
	if (m_nPaddingPixelsOnEnd != 0)
	{
		PadPixels(m_nPaddingPixelsOnEnd);
		m_nPaddingPixelsOnEnd = 0 ;
	}

	if (GetWriteBuffer()->GetSize() != GetExpectedSize())
	{
		xnLogWarning(XN_MASK_SENSOR_READ, "Read: Depth buffer is corrupt. Size is %u (!= %u)", GetWriteBuffer()->GetSize(), GetExpectedSize());
		FrameIsCorrupted();
	}

	// now add the size of the shifts map (appended at the end)
	GetWriteBuffer()->UnsafeUpdateSize(GetWriteBuffer()->GetSize());

	// call base
	XnFrameStreamProcessor::OnEndOfFrame(pHeader);
}

void XnDepthProcessor::PadPixels(XnUInt32 nPixels)
{
	XnBuffer* pWriteBuffer = GetWriteBuffer();

	// check for overflow
	if (!CheckDepthBufferForOverflow(nPixels * sizeof(XnDepthPixel)))
	{
		return;
	}

	XnDepthPixel* pDepth = GetDepthOutputBuffer();
	XnDepthPixel* pShift = GetShiftsOutputBuffer();

	// place the no-depth value
	for (XnUInt32 i = 0; i < nPixels; ++i, ++pDepth, ++pShift)
    {
		*pDepth = GetStream()->GetNoDepthValue();
		*pShift = 0;
    }
	pWriteBuffer->UnsafeUpdateSize(nPixels * sizeof(XnDepthPixel));
}

void XnDepthProcessor::OnFrameReady(XnUInt32 nFrameID, XnUInt64 nFrameTS)
{
	XnFrameStreamProcessor::OnFrameReady(nFrameID, nFrameTS);

	m_pDevicePrivateData->pSensor->GetFPSCalculator()->MarkInputDepth(nFrameID, nFrameTS);
}
