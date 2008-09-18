/* 
 * The preferences dialog.
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
import java.util.Vector;
import java.util.prefs.Preferences;

import org.eclipse.swt.widgets.Shell;

public class Prefs {
	public static final int NR_RECENT_FILENAMES = 8;
	
	public static final int DEFAULT_X = 768;
	public static final int DEFAULT_Y = 576;
	
	public static final int DEFAULT_FORMAT = Utils.FORMAT_YUV;
	
	Preferences mPrefs;
	Preferences mRecentFiles;
	Preferences mDefaultInfo;
	Preferences mFileInfo;
	
	// Used to notify errors.
	Shell mNotifyShell;
	
	Prefs(Shell notifyShell)
	{
		mPrefs = Preferences.userNodeForPackage(Prefs.class);
		mPrefs = mPrefs.node("YUV2");
		mRecentFiles = mPrefs.node("RecentFiles");
		mDefaultInfo = mPrefs.node("DefaultInfo");
		mFileInfo = mPrefs.node("FileInfo");
		mNotifyShell = notifyShell;
	}
	
	/** Flush all pending writes to the backing
	 *   store in preparation for closing the application.
	 *
	 */
	void flush(Shell inShell)
	{
		try
		{
			mPrefs.flush();
		}
		catch (Exception e)
		{
			Utils.showMessageBox(inShell, 
					"Cannot write preferences - " + e.getMessage());
		}
	}
	
	/** Retrieve a list of the recent files, up to 
	 *  NR_RECENT_FILENAMES
	 */
	Vector getRecentFiles()
	{
		Vector recentVector = new Vector();
		
		for (int i = 0 ; i < NR_RECENT_FILENAMES; ++i)
		{
			String currentValue = mRecentFiles.get(Integer.toString(i), null);
			
			if (currentValue != null)
			{
				recentVector.addElement(currentValue);
			}
		}
		return recentVector;
	}
	
	/** Modify the preferences file to register that
	 *   we just opened this file.
	 */
	void registerFileOpen(String inFilename)
	{
		/* We just rewrite the whole array - it's less
		 *  error prone, and config file writes are cheap.
		 */
		Vector recentVector = new Vector();
		recentVector.addElement(inFilename);
		
		File mostRecentFile = new File(inFilename);
		
		for (int i=0;i< NR_RECENT_FILENAMES-1;++i)
		{
			String currentValue = mRecentFiles.get(Integer.toString(i), null);
			
			if (currentValue != null)
			{
				File currentFile = new File(currentValue);

				// Push duplications to the top.
				if (!currentFile.equals(mostRecentFile))
				{
					recentVector.addElement(currentValue);
				}
			}
		}
		
		/* Now write back .. */
		for (int i=0;i < recentVector.size(); ++i)
		{
			mRecentFiles.put(Integer.toString(i), (String)recentVector.elementAt(i));
		}
	}

	
	/** A subclass which stores file association info
	 */
	public static class FileDimensionInfo
	{
		String mFilename;
		int mHeight;
		int mWidth;
		int mFormat;
		
		FileDimensionInfo()
		{
			mFilename = null; mWidth = DEFAULT_X; mHeight = DEFAULT_Y;
			mFormat = DEFAULT_FORMAT;
		}
		
		FileDimensionInfo(Preferences inPrefs)
		{
			read(inPrefs);
		}
		
		FileDimensionInfo(String inFilename, int inHeight, int inWidth,
					int inFormat)
			{
				mFilename = inFilename; mHeight = inHeight;
				mWidth = inWidth; mFormat = inFormat;
			}
		
		void read(Preferences inPrefs)
		{
			mFilename = inPrefs.get("filename", null);
			mWidth = inPrefs.getInt("x", DEFAULT_X);
			mHeight = inPrefs.getInt("y", DEFAULT_Y);
			mFormat = inPrefs.getInt("format", DEFAULT_FORMAT);
		}
		
		void write(Preferences inPrefs)
		{
			if (mFilename != null)
				{
				inPrefs.put("filename", mFilename);
				}
			inPrefs.putInt("x", mWidth);
			inPrefs.putInt("y", mHeight);
			inPrefs.putInt("format", mFormat);
		}
	};
	
	/** Retrieve default dimensions for this file, or
	 *   the global default if it doesn't appear in the list.
	 */
	FileDimensionInfo getDimensions(String inFilename)
	{
		if (inFilename != null)
		{
			File inFile = new File(inFilename);
			
	
			
			for (int i=0;i < NR_RECENT_FILENAMES; ++i)
			{
			try
			{
			
				if (mFileInfo.nodeExists(Integer.toString(i)))
				{
					FileDimensionInfo cur = 
						new FileDimensionInfo
						(mFileInfo.node(Integer.toString(i)));
					File fileB = new File(cur.mFilename);
					
					
					if (inFile.equals(fileB))
					{
						// Gotcha.
						return cur;
					}
				}
			}
			catch (Exception e)
			{ 
				// Ignore
			}
			}
		
		}

		// Bugger.
		return new FileDimensionInfo(mDefaultInfo);
	}
	
	
	/** We just set dimensions - store them for future use as
	 *   the default, and associate them with this file in the
	 *   recent dimensions list.
	 *   
	 *   This way of doing things is a little clumsy, but it 
	 *   at least maintains some sort of robustness in the face
	 *   of concurrent invocations.
	 */
	void setDimensions(FileDimensionInfo curDim)
	{
		curDim.write(mDefaultInfo);
		
		if (curDim.mFilename == null)
		{
			return;
		}
		
		Vector newFileInfo = new Vector();
		File newTopFile = new File(curDim.mFilename);
	
		newFileInfo.addElement(curDim);
		
		/* Now load up the recent files list */
		for (int i=0;i<NR_RECENT_FILENAMES-1;++i)
		{
			try
			{
				if (mFileInfo.nodeExists(Integer.toString(i)))
				{
				FileDimensionInfo newDim = new FileDimensionInfo
					(mFileInfo.node(Integer.toString(i)));
				File curFile = new File(newDim.mFilename);
				
				if (!curFile.equals(newTopFile))
				{
					newFileInfo.addElement(newDim);
				}
			}	
			} catch (Exception e)
			{
				// Ignore...
			}
		}
		
		for (int i=0;i<newFileInfo.size();++i)
		{
			((FileDimensionInfo)newFileInfo.elementAt(i)).write
				(mFileInfo.node(Integer.toString(i)));
		}
	}
}

// End file.

