/* 
 * A canvas for painting BufferedImages on.
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


import java.io.File;
import java.io.RandomAccessFile;
import java.util.Formatter;
import java.util.Locale;

import org.eclipse.swt.SWT;
import org.eclipse.swt.events.DisposeEvent;
import org.eclipse.swt.events.DisposeListener;
import org.eclipse.swt.events.MouseEvent;
import org.eclipse.swt.events.MouseListener;
import org.eclipse.swt.events.MouseMoveListener;
import org.eclipse.swt.events.PaintEvent;
import org.eclipse.swt.events.PaintListener;
import org.eclipse.swt.graphics.Color;
import org.eclipse.swt.graphics.Image;
import org.eclipse.swt.graphics.ImageData;
import org.eclipse.swt.graphics.PaletteData;
import org.eclipse.swt.graphics.Point;
import org.eclipse.swt.widgets.Canvas;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Event;
import org.eclipse.swt.widgets.Listener;
import org.eclipse.swt.widgets.Shell;

public class ImageCanvas extends Canvas {
	Color mBg, mWhite;
	Shell mShell;
	int mWidth, mHeight;
	int mFormat;
	int mFrameNumber;
	long mFrameTotal;
	int mZoomNumerator, mZoomDenominator;
	boolean mIsFieldMode;
	boolean mMBMarkers;
	RandomAccessFile mReader;
	public String mFilename;
	MainFrame mContainingFrame;
	int mMacroblockInfoCounter;
	boolean mPlayMode;
	int mPlayFrame;
	int mPlayTo;
	BufferedImage mCurrentImage;
	BufferedImage mNextImage;
	boolean mNextImagePending;
	
	
	static class PlayThread extends Thread
	{
		ImageCanvas mCanvas;
		
		public PlayThread(ImageCanvas inCanvas)
		{
			mCanvas = inCanvas;
		}
		
		public void run()
		{
			while (true)
			{
				int sleepTime = 40; // Inter-frame time.
				
				
				synchronized (mCanvas)
				{
					if (mCanvas.mKillAnimationThread)
					{
						return;
					}
					if (mCanvas.mPlayMode && 					
					(mCanvas.mFrameNumber < mCanvas.mFrameTotal && 
					(mCanvas.mPlayTo == -1 ||
						mCanvas.mFrameNumber < mCanvas.mPlayTo)))
					{
						boolean done_something = false;
						
						// We're not allowed to do this directly, so.
						if (!mCanvas.mAdvancePending && 
							 !mCanvas.mNextImagePending)
							{
							YUV2.addWork(new YUV2.WorkRequest
									(mCanvas.mContainingFrame,
								YUV2.WorkRequest.ADVANCE_PLAY));
							mCanvas.mAdvancePending = true;								
							done_something = true;
							}
						
						if (!mCanvas.mNextImagePending)
						{
							mCanvas.mNextImagePending = true;
							YUV2.addWork(new YUV2.WorkRequest
									(mCanvas,
									YUV2.WorkRequest.BUFFER_NEXT_IMAGE));		
							done_something = true;
						}								
						
						if (!done_something)
						{
							// We wanted to draw, but couldn't. Check
							// again soon!
							sleepTime = 4;
						}
					}
					else
					{
						// We're stopped. Wait until we're turned on again.
						YUV2.addWork(new YUV2.WorkRequest
									(mCanvas.mContainingFrame,
											YUV2.WorkRequest.STOP_PLAY));
						
						try
						{
							mCanvas.wait();
						} 
						catch (InterruptedException ie)
						{
							// Might as well check now ..
						}
						
					}
				}
				
				// Sleep for the inter-frame time, which is 1/25s = 
				// 4ms.
				try
				{
					sleep(sleepTime);
				} catch (InterruptedException ie)
				{
					// Meh.
				}
			}
		}
	}

	// The actual play thread ..
	PlayThread mPlayThread;
	boolean mAdvancePending;
	boolean mKillAnimationThread;
	
	ImageCanvas(Composite inParent, 
			MainFrame inContainingFrame,
				Shell inShell,
				int style)
	{
		super(inParent, style + SWT.NO_BACKGROUND);	
	    mContainingFrame = inContainingFrame;
	    mMacroblockInfoCounter = 0;
		mShell = inShell;
		mWidth = mHeight = 0;
		mFrameNumber = 0;
		mFrameTotal = 0;
		mFormat = 0;
		mIsFieldMode = false;
		mZoomNumerator = 1; mZoomDenominator = 1;		
		mMBMarkers = false;
		mAdvancePending = false;
		mKillAnimationThread = false;
		mBg = new Color(null, 255, 255, 255);
		mWhite = new Color(null, 255, 255, 255);
		// Create the play thread.
		setPlayMode(false);
		mPlayTo = -1;
		mPlayFrame = 0;
		mNextImagePending = false;
		
		mCurrentImage = mNextImage = null;
		
		mPlayThread = new PlayThread(this);
		mPlayThread.start();
		setBackground(mBg);
		addDisposeListener(new Disposer());
		addPaintListener(new Painter());
		addMouseListener(new ClickLogger());
		addListener(SWT.Resize, 
				new Listener() 
				{
				public void handleEvent(Event ev)
				{
				  
				}
	 });
		addMouseMoveListener(new Mouser());
	}

	// Instruct our animation thread to die.
	public void prepareForExit()
	{
		synchronized (this)
		{
			mKillAnimationThread = true;
			notifyAll();
		}
	}
	
	/** Set the play mode
	 * 
	 */
	void setPlayMode(boolean inValue)
	{
		synchronized (this)
		{
			mPlayMode = inValue;
			mPlayFrame = 0;
			this.notifyAll();
		}
	}

	
	/** Attempt to close a reader, emitting an error
	 *   dialog if we fail.
	 *   
	 * @param reader The reader to close.
	 */
	void closeReader(RandomAccessFile reader)
	{
		
		// Do we have anything to do at all?
		if (reader == null)
		{
			return;
		}

		try
		{
			reader.close();
		}
		catch (Exception ioe)
		{
			Utils.showMessageBox(mShell,
					"Cannot close " + mFilename + " - " + 
						ioe.getMessage());
		}
	}
	
	RandomAccessFile getReader()
	{
		
		if (mFilename == null)
		{
			return null;
		}
		
		File aFile = new File(mFilename);
		RandomAccessFile rv = null;
		
		if (!aFile.exists())
		{
				Utils.showMessageBox(mShell,
							mFilename +  " does not exist");
				return null;			
		}
		/* Open the file .. */
		try
		{
			rv = new RandomAccessFile(aFile, "r");
		}
		catch (Exception e)
		{
			Utils.showMessageBox
			(mShell,
			"Cannot open " + mFilename + " - " + e.getMessage());
			return null;
		}
		
		return rv;
	}
	
	/** Opens a file. If we have X and Y coordinates,
	 *  we will attempt to display the file.
	 *
	 * @return true if we succeeded, false if we didn't
	 *  and displayed an error box.
	 */
	boolean openFile(String fileName)
	{	
		File theFile = new File(fileName);
		
		if (!theFile.exists())
		{
			Utils.showMessageBox(mShell,
					fileName + " does not exist.");
			return false;
		}
		
		RandomAccessFile reader = null;
		
		try
		{
			reader = new RandomAccessFile(theFile,
						"r");
		}
		catch (Exception e)
		{
			Utils.showMessageBox
			(mShell,
			"Cannot open " + fileName + " - " + e.getMessage());
			return false;
		}
		
		/* Otherwise .. */
		if (reader != null) 
		{
			closeReader(reader);
		}

		mFilename = fileName;
		updateImage();
		return true;
	}
	
	/** Change the frame number we're looking at, and
	 *    update the image.
	 */
	void changeFrameNumber(int inFrameNumber)
	{
		if (inFrameNumber == mFrameNumber)
		{
			return;
		}
		mFrameNumber = inFrameNumber;
		mContainingFrame.transientInfo("Go to frame " +
				Integer.toString(mFrameNumber));
		updateImage();
	}
	
	/** Change the width, height and format settings, and
	 *   update the image.
	 */
	void changeFormat(int inWidth, int inHeight, int inFormat)
	{
		if (inWidth == mWidth && inHeight == mHeight && 
			inFormat == mFormat)
		{
			// Nothing to do.
			return;
		}
		
		mWidth = inWidth; mHeight = inHeight;
		mFormat = inFormat;
		
		mContainingFrame.note("Changed format: " +
					Integer.toString(mWidth) + "x" + 
					Integer.toString(mHeight) + " " +
					Utils.formatToString(mFormat));
		
		updateImage();
	}

	void makeNextImage()
	{
		if (mNextImage != null)
		{
			mNextImage.dispose();
		}
		
		mNextImage =new BufferedImage(mFrameNumber + 1, this);
		mNextImage.convertImage(true);
		
		synchronized (this)
		{
			mNextImagePending = false;
		}
	}
	
	
	/** Try to read in the required image from the
	 *   given data.
	 *   
	 *   @return true if we managed to synthesise an
	 *     image, false if we didn't.
	 */
	boolean updateImage()
	{
		// Force a repaint.
		this.redraw();		

		synchronized (this)
		{
		if (mNextImage != null && 
		     mNextImage.mFrameNumber == mFrameNumber)
		{
				// We already have one :-).
	    	mCurrentImage.dispose();
	    	mCurrentImage = mNextImage;
	    	mFrameNumber = mNextImage.mFrameNumber;
	    	mNextImage = null;
	    	if (!mCurrentImage.mReadyForDrawing)
	    	{
	    		this.redraw();
	    		return false;
	    	}
		}
		else
		{
			if (mCurrentImage != null)
			{
				mCurrentImage.dispose();
			}
			mCurrentImage = new BufferedImage(mFrameNumber, this);
			
		
			if (!mCurrentImage.convertImage(false))
			{
				this.redraw();			
				return false;
			}
		}
		}
		
		// Resize ourselves, incidentally updating the status bar.
		this.updateSize();
			

		
		// The redraw itself may not happen, so setting advancePending here
		// is the least worst we can do.
		synchronized (this)
    	{
			mAdvancePending = false;
    	}
		return true;
		
	}
	
	/** Update the size of this widget in response to an
	 *   image update or a zoom level update.
	 */
	void updateSize()
	{
		int actualWidth = (mWidth * mZoomNumerator) / mZoomDenominator;
		int actualHeight = (mHeight * mZoomNumerator) / mZoomDenominator;
		
		if (actualWidth == 0 && actualHeight == 0)
		{
			actualWidth = 720;
			actualHeight = 576;
		}
		
		Point currentSize = this.getSize();
		
		if (actualWidth != currentSize.x ||
					actualHeight != currentSize.y)
		{
			this.setSize(actualWidth, actualHeight);
		}

		this.setTitle();
		mContainingFrame.updateStatus();
	}
	
	public void dispose()
	{
	    if (mCurrentImage != null) 
	    {
	    		mCurrentImage.dispose(); 
	    }
	    if (mNextImage != null)
	    {
	    	mNextImage.dispose();
	    }
		super.dispose();
	}

	// Local function: set the window title to something
	//  descriptive.
	void setTitle()
	{
		StringBuffer aTitle = new StringBuffer();
		
		if (mFilename == null)
		{
			aTitle.append("YUV2 - No file loaded ");
		}
		else
		{
			aTitle.append("YUV2 - ");
			aTitle.append(mFilename);
			aTitle.append(" ");
			aTitle.append(Integer.toString(mWidth));
			aTitle.append("x");
			aTitle.append(Integer.toString(mHeight));
			aTitle.append(" ");
		}
		aTitle.append(" F:"); 
		aTitle.append(Integer.toString(mFrameNumber));
		aTitle.append("/");
		aTitle.append(Long.toString(mFrameTotal-1));
		aTitle.append(" Z:");
		aTitle.append(Integer.toString(mZoomNumerator));
		aTitle.append("/");
		aTitle.append(Integer.toString(mZoomDenominator));
		
		mShell.setText(aTitle.toString());
	}
	
	public void SetZoomNumerator(int inNumerator)
	{
		mZoomNumerator = inNumerator;
	
		mContainingFrame.transientInfo("Set zoom " + 
				Integer.toString(mZoomNumerator) + ":" +
				Integer.toString(mZoomDenominator));
	
		this.updateSize();
		this.redraw();
	}

	public void SetZoomDenominator(int inDenominator)
	{
		if (inDenominator < 1)
		{
			// Ha ha. Very funny :-)
			return;
		}
		mZoomDenominator = inDenominator;
		
		mContainingFrame.note("Set zoom " + 
				Integer.toString(mZoomNumerator) + ":" + 
				Integer.toString(mZoomDenominator));

		this.updateSize();
		this.redraw();
	}
	
	/** Get Info about a particular pixel location in the widget.
	 * 
	 */
	StringBuffer getPixelInfo(int x, int y)
	{
		StringBuffer rv = new StringBuffer();
		int pixel_x = (x * mZoomDenominator) / mZoomNumerator;
		int pixel_y = (y * mZoomDenominator) / mZoomNumerator;
		Point mb_frame = new Point(pixel_x/16, pixel_y/16);
		Point mb_field = new Point(pixel_x/16, (pixel_y >> 1)/16);
		
		rv.append("("); rv.append(Integer.toString(pixel_x));
		rv.append(",");
		rv.append(Integer.toString(pixel_y));
		rv.append(") MB "); 
		if (mIsFieldMode)
			{
			 rv.append("<" + Integer.toString(mb_field.x) + "," + 
		        		Integer.toString(mb_field.y) + "," + 
		        		Integer.toString(((pixel_y & 16) != 0)? 1 : 0) + ">");
			 rv.append(" @" + Integer.toString(pixel_x%16) + "," + 
					 Integer.toString((pixel_y>>1)%16));
			}
		else
		{
			rv.append("( "  + Integer.toString(mb_frame.x) + "," +
				Integer.toString(mb_frame.y) + ")");
			rv.append(" @" + Integer.toString(pixel_x%16) + "," + 
					Integer.toString(pixel_y%16));
		}
		
		
		
        rv.append(" = ");
        if (pixel_x < mWidth && pixel_y <= mHeight && 
        		pixel_x >= 0 && pixel_y >= 0)
        {
        	int yoffset = (pixel_y * mWidth) + pixel_x;
        	int uvoffset = ((pixel_y>>1) * (mWidth >> 1)) + (pixel_x>>1);
           	rv.append("0x" +  Integer.toString(mCurrentImage.mYBuf[yoffset]&0xff, 16) + " ");
           	rv.append("0x" +  Integer.toString(mCurrentImage.mUBuf[uvoffset]&0xff, 16) + " ");
           	rv.append("0x" +  Integer.toString(mCurrentImage.mVBuf[uvoffset]&0xff, 16) + ">");
        }
        return rv;
        
	}
	
	boolean hasValidImage()
	{
		return ((mCurrentImage != null) && 
			(mCurrentImage.mImage != null) && 
		(mCurrentImage.mUBuf != null) && 
		(mCurrentImage.mVBuf != null) &&
		(mCurrentImage.mYBuf != null));
	}
	
	/** The user dragged on (x,y). Show them some information about
	 *   it.
	 *   
	 * @param x
	 * @param y
	 */
	void dropper(int x, int y, boolean clicked)
	{
		if (!hasValidImage())
		{
			mContainingFrame.transientInfo
				("No current image");
			return;
		}
		
		StringBuffer rv = getPixelInfo(x,y);
		if (!clicked)
			{
				mContainingFrame.transientInfo(rv.toString());
			}
		else
		{
			mContainingFrame.note(rv.toString());
		}
			
	}
	
	/** The user double-clicked; dump macroblock info.
	 * 
	 * 
	 * 	
	 *  */
	void dumpMacroblock(int click_x, int click_y)
	{
		if (!hasValidImage())
		{
			mContainingFrame.transientInfo
				("No current image");
			return;
		}
		
		StringBuilder logInfo = new StringBuilder();

		int pixel_x = (click_x * mZoomDenominator) / mZoomNumerator;
		int pixel_y = (click_y * mZoomDenominator) / mZoomNumerator;
	
		logInfo.append(Integer.toString(mMacroblockInfoCounter) + 
				" : Macroblock info at pixel " + 
				Integer.toString(pixel_x) + "," +
				Integer.toString(pixel_y));
	
		++mMacroblockInfoCounter;
		
		if (pixel_x < 0 || pixel_y < 0 ||
				pixel_x >= mWidth || pixel_y >= mHeight)
		{
			logInfo.append(": Coordinates out of range.");
		}
		else
		{
			// Actually do it.
			Point mb_coords = new Point(0,0);
			boolean topField;
			boolean inFieldMode = this.mIsFieldMode;
		// Formatter requires us to use a dummy array for its arguments.
			Object tmpO[] = new Object[1];
			
			if (inFieldMode)
			{
				mb_coords.x = (pixel_x >> 4);
				mb_coords.y = (pixel_y&~1)>>4;
				topField = ((pixel_y&1) == 0) ? true : false;
				logInfo.append(" Field MB <" + 
						Integer.toString(mb_coords.x) + "," + 
						Integer.toString(mb_coords.y) + "," +
						(topField ? "0" : "1") + ">:\n");
				
				// Now dump the MB
				{
					int y_offset = ((mb_coords.y * 32) + 
									(topField ? 0 : 1)) * mWidth + 
									mb_coords.x * 16;
					
					// the -16 is here to cope with macroblocks that run just
					//  off the end of the array.
					int y_max = ((mWidth) * mHeight) - 16;
					int uv_offset = ((mb_coords.y * 16) + 
									 (topField ? 0 : 1)) * (mWidth >> 1) + 
									(mb_coords.x * 8);
					int uv_max = ((mWidth>>1)*(mHeight>>1))-8;
					Formatter formatter = new Formatter(logInfo, Locale.UK);
					
					
					logInfo.append("Y:\n");
					
					// y_offset += mWidth *2 because we're in field mode.
					for (int y = 0; y < 16 && y_offset <= y_max; ++y, 
						y_offset += mWidth*2)
					{
						for (int x = 0; x < 16; ++x)
						{
							tmpO[0] = new Byte(mCurrentImage.mYBuf[y_offset + x]);
							formatter.format("%02x ", tmpO);
						}
						logInfo.append("\n");
					}
					logInfo.append("\n");
					logInfo.append("U:\n");
					for (int y = 0, uv_cur = uv_offset; 
						y < 8 && uv_cur <= uv_max; 
						++y, uv_cur += mWidth)
					{
						for (int x = 0; x < 8; ++x)
						{
							tmpO[0] = new Byte(mCurrentImage.mUBuf[uv_cur + x]);
							formatter.format("%02x ", tmpO);
	
						}
						logInfo.append("\n");
					}
					logInfo.append("\n");
					logInfo.append("V:\n");
					for (int y = 0, uv_cur = uv_offset;
						 y < 8 && uv_cur <= uv_max; 
						 ++y, uv_cur += mWidth)
					{
						for (int x = 0; x < 8; ++ x)
						{
							tmpO[0] = new Byte(mCurrentImage.mVBuf[uv_cur + x]);
							formatter.format("%02x ", tmpO);
						}
						logInfo.append("\n");
					}
				}
			}
			else
			{
				mb_coords.x = (pixel_x >> 4);
				mb_coords.y = (pixel_y >> 4);
				topField = true;
				logInfo.append(" Frame MB (" + 
							Integer.toString(mb_coords.x) + "," + 
							Integer.toString(mb_coords.y) + "):\n");

				
				/* Dump the macroblock */
				
				{
					int y_offset = ((mb_coords.y * 16)*mWidth) + 
								mb_coords.x*16;
					int y_max = (mWidth * mHeight) - 16;
					int uv_offset = ((mb_coords.y * 8) * (mWidth >> 1)) + 
						mb_coords.x*8;
					int uv_max = ((mWidth >> 1) * (mHeight >> 1)) - 8;
					Formatter formatter = new Formatter(logInfo, Locale.UK);
					
					logInfo.append("Y:\n");
					for (int y = 0; y < 16 && y_offset <= y_max; ++y,
						y_offset += mWidth)
					{
						for (int x = 0; x < 16 ; ++ x)
						{
							tmpO[0] = new Byte(mCurrentImage.mYBuf[y_offset + x]);
							formatter.format("%02x ", tmpO);
						}
						logInfo.append("\n");
					}
					
					logInfo.append("\n");
					logInfo.append("U:\n");
					for (int y=0, uv_cur = uv_offset;
					 	y < 8 && uv_cur <= uv_max; 
					 	++y, uv_cur += mWidth >> 1)
					{
						for (int x = 0; x<8;++x)
						{
							tmpO[0] = new Byte(mCurrentImage.mUBuf[uv_cur + x]);
							formatter.format("%02x ", tmpO);
						}
						logInfo.append("\n");
					}
					logInfo.append("\n");
					logInfo.append("V:\n");
					for (int y=0, uv_cur = uv_offset;
					 	y < 8 && uv_cur <= uv_max; 
					 	++y, uv_cur += mWidth >> 1)
					{
						for (int x = 0; x<8;++x)
						{
							tmpO[0] = new Byte(mCurrentImage.mVBuf[uv_cur + x]);
							
							formatter.format("%02x ", tmpO);
						}
						logInfo.append("\n");
					}
					logInfo.append("\n");
				}
			}
			
			
		}
		
		// Log everything ..
		mContainingFrame.log(logInfo.toString());
		
	}
	
	
	void setFieldMode(boolean inIsField)
	{
		mIsFieldMode = inIsField;
		this.setTitle();
		mContainingFrame.updateStatus();
		this.redraw(); // << Markers may need updating.
	}
	void setMBMarkers(boolean inMBMarkers)
	{
		mMBMarkers = inMBMarkers;
		this.setTitle();
		mContainingFrame.updateStatus();
		redraw();
	}
	
	
	class ClickLogger implements MouseListener
	{
		public void mouseDoubleClick(MouseEvent e)
		{
			// Got a double-click; dump this macroblock to the log.
			dumpMacroblock(e.x, e.y);
		}
		public void mouseDown(MouseEvent e)
		{
			// Double-click is almost always what you want, and the
			// single-click behaviour messes up the logs :-(.
			//	dropper(e.x, e.y, true);
		}
		public void mouseUp(MouseEvent e)
		{
			// Do nothing.
		}
	}
	
	class Mouser implements MouseMoveListener
	{

		public void mouseMove(MouseEvent theEvent)
		{
			/* If you move the mouse over a pixel, and you have a 
			 *  button held down, we activate the dropper.
			 */
			dropper(theEvent.x, theEvent.y, false);
		}
	}
	
	class Disposer implements DisposeListener
	{
		public void widgetDisposed(DisposeEvent e)
		{
			mBg.dispose();
		}
	}
	
	class Painter implements PaintListener
	{
		public void paintControl(PaintEvent e)
		{
			int hDimension = (mWidth * mZoomNumerator)/ mZoomDenominator;
			int vDimension = (mHeight * mZoomNumerator) / mZoomDenominator;

			// Just to prove we exist :-).
			e.gc.setBackground(mBg);
			e.gc.setForeground(mWhite);
			
			synchronized (this)
			{
				if (mCurrentImage != null && 
				    mCurrentImage.mImage != null)
				{
					e.gc.drawImage(mCurrentImage.mImage, 0, 0, mWidth, mHeight, 
						    	0, 0, 
						    	hDimension, vDimension);
				}
				else
				{
					e.gc.fillRectangle(e.x, e.y, e.width,e.height);
				}
			}
			
			/* Any macroblock markers? */
			if (mMBMarkers)
			{
				/* Work out how big we're supposed to draw them .. */
				int hStep = (16 * mZoomNumerator) / mZoomDenominator;
				int vStep = (16 * mZoomNumerator) / mZoomDenominator;
				
				/* If we're in field mode, double the Y stride 
				 * (actually, anything we do here will be wrong, but
				 *  Rhodri reckons this is the least wrong thing to do,
				 *  and I agree)
				 */
				if (mIsFieldMode)
				{ 
					vStep <<= 1; 
				}
				
				/* If either of our steps were going to be zero, 
				 *  don't bother drawing anything - we'll just obliterate
				 *  the image.
				 */
				if (hStep != 0 && vStep != 0)
				{
					e.gc.setXORMode(true);
					for (int j=0;j<=vDimension;j += vStep)
					{
						/* Draw the horizontals */
						e.gc.drawLine(0, j, hDimension, j);
					}
					
					/* Draw the verticals. We get cancellation 
					 * at the edges, but this isn't too much of
					 * a problem.
					 */
					for (int i=0;i<=hDimension;i += hStep)
					{
						e.gc.drawLine(i, 0, i, vDimension);
					}
					e.gc.setXORMode(false);
				}
			}
			
	
		}
	}
	
}

/* End file */

