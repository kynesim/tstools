/* 
 * Display an image, performing colourspace conversion as we do so.
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the MPEG TS, PS and ES tools.
 *
 * The Initial Developer of the Original Code is Amino Communications Ltd.
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Amino Communications Ltd, Swavesey, Cambridge UK
 *   Kynesim, Cambridge UK
 *
 * ***** END LICENSE BLOCK *****
 */


package com.aminocom.yuv2;

import java.io.RandomAccessFile;

import org.eclipse.swt.graphics.Image;
import org.eclipse.swt.graphics.ImageData;
import org.eclipse.swt.graphics.PaletteData;

/** This class buffers a frame from a file in the hope that
 *  we can make animation a bit smoother.
 *  
 * @author rrw
 *
 */
public class BufferedImage {
	ImageCanvas mParentCanvas;
	MainFrame mContainingFrame;
	int mFrameNumber;
	boolean mReadyForDrawing;
	
	/** The image data to paint. This is updated
	 *  whenever we do anything by updateImageData().
	 *
	 * When we have no image data, we set this to NULL
	 *  and paint plain white.
	 */
	ImageData mImageData;

	/** The image to paint, if present.
	 */
	Image mImage;

	/** Y frame buffer. Used in image conversion and
	 *  the dropper tool. 
	 *  Frame buffers are stored in ordinary raster format.
	 */
	byte[] mYBuf, mUBuf, mVBuf;
	
	BufferedImage(int inFrameNumber, 
				ImageCanvas inParent)				
	{
		mParentCanvas = inParent;
		mContainingFrame = inParent.mContainingFrame;
		mFrameNumber = inFrameNumber;
		mReadyForDrawing = false;
	}
	
	void dispose()
	{
		if (mImage != null)
		{
				mImage.dispose();
		}
		mImage = null; mImageData = null;
	}
	
	boolean convertImage(boolean inQuiet)
	{
		RandomAccessFile reader = mParentCanvas.getReader();
		
		
		if (reader == null)
		{
			// No file ..
			return false;
		}
		// Start off by killing any old data ..
		// We actually do want to do this here - the last thing the
		//  user wants is to navigate to a frame and have us redisplay
		//  the last one they visited.
		
		if (mImage != null)
		{
			mImage.dispose();
		}
		mImageData = null;
		mImage = null;
		
		if (mParentCanvas.mWidth == 0 || 
			mParentCanvas.mHeight == 0)
		{
			// Bugger. No dimensions. Have to wait
			//  until we have some.
			mContainingFrame.updateStatus();
			mParentCanvas.closeReader(reader);
			return false;
		}
	
		// frame size = width * height for luma, half that for chroma.
		long frameSize = (mParentCanvas.mWidth * 
					mParentCanvas.mHeight * 3)>>1;
		long fileOffset = frameSize*((long)mFrameNumber);
	
		if (!inQuiet)
			{
			mContainingFrame.log("Frame Size = 0x" +
					Long.toString(frameSize) + " file offset for frame " +
					Integer.toString(mFrameNumber) + " = 0x" +
					Long.toString(fileOffset, 16));
			}
		
		try
		{
			long fileLen = reader.length();
			
			mParentCanvas.mFrameTotal = fileLen/frameSize;
			mContainingFrame.setNumFrames
			(mParentCanvas.mFrameTotal);
			
			if (fileOffset > fileLen)
			{
				// Can't display this frame.
				mContainingFrame.note("Attempt to read frame past end of file: " +
						"frame " + Integer.toString(mFrameNumber) + 
						"@" + Long.toString(fileOffset) + 
						" > file len " + Long.toString(fileLen)); 
				mContainingFrame.updateStatus();
				mParentCanvas.closeReader(reader);
				return false;
			}
		
			if (fileOffset + frameSize > fileLen)
			{	
				mContainingFrame.note("File ends between frame " +
						Integer.toString(mFrameNumber) + " and " +
						Integer.toString(mFrameNumber+1));
			}
		
			reader.seek(fileOffset);
		}
		catch (Exception e)
		{
			mContainingFrame.updateStatus();
			Utils.showMessageBox(mParentCanvas.mShell,
					"Can't read frame from input file.");
		}
		

		// Right. Read as much data as we can into the
		//  relevant buffers. Our YUV file format is 
		//  Y, then U, then V, in separate chunks.
		int array_height = mParentCanvas.mHeight;
		
		// Round the array size up to a multiple of two lines so
		// our conversion routine will run at a sensible speed.
		if (array_height%1 != 0)
		{
			++array_height;
		}
		
		
		mYBuf = new byte[mParentCanvas.mWidth * array_height];
		mUBuf = new byte[(mParentCanvas.mWidth>>1)*  (array_height>>1)];
		mVBuf = new byte[(mParentCanvas.mWidth>>1) * (array_height>>1)];
		
		try
		{
			reader.readFully(mYBuf);
			reader.readFully(mUBuf);
			
			reader.readFully(mVBuf);
		}
		catch (Exception e)
		{
			mContainingFrame.note("Didn't get whole frame (" + 
					e.getMessage() + ")");
		}
		
		// Now convert to RGB ...
				
		// Internally, we'll use standard RGBA, so
		//  Red = 0xff, Green = 0xff00, blue = 0xff0000
		mImageData = new ImageData
			(mParentCanvas.mWidth, mParentCanvas.mHeight,
				32, new PaletteData(0xff0000, 0xff00,
						0xff));
		
		// We'll convert 2 lines to make copying somewhat
		// quicker (2 lines means we can cache the chroma).
		
		int[] outgoingLine = new int[2*mParentCanvas.mWidth];
		
		// The input is just a standard, line-by-line
		// 4:2:0 chroma file.
		
		if (!inQuiet)
			{
			mContainingFrame.transientInfo("Converting YUV image "  +
					Integer.toString(mParentCanvas.mWidth) + "x" + 
					Integer.toString(mParentCanvas.mHeight) + "...");
			}
		for (int i=0;i<mParentCanvas.mHeight;i += 2)
		{
			int ypos = i * mParentCanvas.mWidth;
			int uvpos = (i>>1)*(mParentCanvas.mWidth>>1);
			
			for (int j=0;j<mParentCanvas.mWidth;++j)
			{
				int y0 = mYBuf[ypos + j]&0xff;
				int y1 = mYBuf[ypos + mParentCanvas.mWidth + j]&0xff;
				int u = mUBuf[uvpos + (j>>1)]&0xff;
				int v = mVBuf[uvpos + (j>>1)]&0xff;
				int r, g, b;
				int ri, gi, bi;
				int C, D, E;
				
			
				
				C = y0 - 16;
				D = u- 128;
				E = v - 128;
				
				r = ((298 * C + 409 * E + 128)>>8);
				g = ((298 * C - 100 * D - 208 * E + 128) >> 8);
				b = ((298 * C + 516 * D + 128)>>8);
				
				if (r < 0) { ri = 0; } else if (r > 255) { ri = 255; } else
					ri = r;
				if (g < 0) { gi = 0; } else if (g > 255) { gi = 255; } else
					gi = g;
				if (b < 0) { bi = 0; } else if (b > 255) { bi = 255; } else
					bi = b;

	
				outgoingLine[j] = (ri << 16) | (gi << 8) | bi;
						
				C = y1 - 16;
				
				r = ((298 * C + 409 * E + 128)>>8);
				g = ((298 * C - 100 * D - 208 * E + 128) >> 8);
				b = ((298 * C + 516 * D + 128)>>8);
				
				if (r < 0) { ri = 0; } else if (r > 255) { ri = 255; } else
					ri = r;
				if (g < 0) { gi = 0; } else if (g > 255) { gi = 255; } else
					gi = g;
				if (b < 0) { bi = 0; } else if (b > 255) { bi = 255; } else
					bi = b;		
		
				outgoingLine[mParentCanvas.mWidth + j] 
				             	= (ri << 16) | (gi << 8) | bi;
			}
			
			
			mImageData.setPixels(0, i, 
						mParentCanvas.mWidth*2, 
						outgoingLine,
						0);
		}
		if (!inQuiet)
			{
			mContainingFrame.transientInfo("Done Conversion.");
			}
		
		// Now we've built the ImageData, build the image ..
		mImage = new Image(null, mImageData);
		
		synchronized (this)
		{
			mReadyForDrawing = true;
		}
		
		mParentCanvas.closeReader(reader);
		
		return true;
	}

	
	
	
}
