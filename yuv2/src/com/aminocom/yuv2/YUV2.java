/* 
 * YUV2 main program.
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

import org.eclipse.swt.widgets.Display;

public class YUV2 {
	public static class WorkRequest
	{
		public static final int UPDATE_IMAGE = 0;
		public static final int STOP_PLAY = 1;
		public static final int ADVANCE_PLAY = 2;
		public static final int BUFFER_NEXT_IMAGE = 3;
		public static final int NOP = 2;
		
		int mCode;
		ImageCanvas mImageCanvas;
		MainFrame mMainFrame;
		WorkRequest next;
		
		WorkRequest()
		{
			mCode = NOP; mImageCanvas = null;
			mMainFrame = null;
			next = null;
		}
		
		WorkRequest(ImageCanvas inObject, int inCode)
		{
			mCode = inCode;
			mImageCanvas = inObject;
			mMainFrame = null;
			next = null;
		}
		
		WorkRequest(MainFrame inFrame, int inCode)
		{
			mCode = inCode;
			mImageCanvas = null;
			mMainFrame = inFrame;
			next = null;
		}
	};
	
	/** A queue of things that need to be done by the UI
	 *  thread.
	 */
	static WorkRequest sDummyHead;
	
	
	/** This routine is safe for multithreading 
	 * We deliberately starve latecomers, first
	 *  because it's easy, second because we'd
	 *  rather give good service to one thread
	 *  and kill everyone else than do a bad
	 *  job everywhere.
	 */
	public static void addWork(WorkRequest newRequest)
	{
		Display responsibleDisplay = 
			(newRequest.mImageCanvas != null ? 
					newRequest.mImageCanvas.getDisplay() :
						newRequest.mMainFrame.mShell.getDisplay());
				
		synchronized (sDummyHead)
		{
			newRequest.next = sDummyHead.next;
			sDummyHead.next = newRequest;
		}
		// Poke the display - new data just came in.
		responsibleDisplay.wake();
	}
	
	
	/** Pop a work request off the queue.
	 * 	 
	 * */
	public static WorkRequest getWork()
	{
		WorkRequest popped = null;
		
		synchronized (sDummyHead)
		{
			if (sDummyHead.next != null)
			{
				popped = sDummyHead.next;
				sDummyHead.next = popped.next;
				popped.next = null; // Just in case.
			}
		}
		return popped;
	}
	
	/** Do a work request
	 */
	public static void DispatchWork(WorkRequest req)
	{		
		switch (req.mCode)
		{
			case WorkRequest.UPDATE_IMAGE: 
				req.mImageCanvas.updateImage();
				break;
			case WorkRequest.ADVANCE_PLAY:
				req.mMainFrame.playAdvance();
				break;
			case WorkRequest.STOP_PLAY:
				req.mMainFrame.playStopped();
				break;
			case WorkRequest.BUFFER_NEXT_IMAGE:
				req.mImageCanvas.makeNextImage();
				break;
		}
	}
	
	public static void main(String[] s)
	{
	    String dimensions = null, filename = null;
	    
	    // Create the dummy work queue head (merely used
	    //  for synchronization).
	    sDummyHead = new WorkRequest();
	    
		Display display = new Display();
	    for (int i=0;i<s.length;++i)
	    {
	    	if (s[i].charAt(0) == '/')
	    	{
	    		dimensions = s[i].substring(1);
	    	}
	    	else
	    	{
	    		filename = s[i];
	    		break;
	    	}
	    }

	    
	    MainFrame mainFrame = new MainFrame(display, dimensions, filename);
	    
	    mainFrame.open();
	    while (!mainFrame.isDisposed()) 
	    {

			if (!display.readAndDispatch())
			{
				// Do we have any video events?
				WorkRequest req = getWork();
				if (req != null)
				{
					DispatchWork(req);
				}

				// There's no need to worry about 
				// this sleeping too long - ImageCanvas's
				// play thread will poke us (via wake())
				// when it needs us.
				display.sleep();
	    	}
	    }
	    display.dispose();
	    
	    
	    // Terminate the process - including the animation thread.
	    System.exit(0);
	}
	
	public static void Exit()
	{
		//System.out.println("Exit handler called");
		// Just kill ..
		System.exit(0);
	}
		
	
}
