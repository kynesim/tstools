/* 
 * The main frame.
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

import java.util.Vector;

import org.eclipse.swt.SWT;
import org.eclipse.swt.custom.ScrolledComposite;
import org.eclipse.swt.events.ModifyEvent;
import org.eclipse.swt.events.ModifyListener;
import org.eclipse.swt.events.SelectionAdapter;
import org.eclipse.swt.events.SelectionEvent;
import org.eclipse.swt.graphics.Point;
import org.eclipse.swt.layout.FormAttachment;
import org.eclipse.swt.layout.FormLayout;
import org.eclipse.swt.layout.RowLayout;
import org.eclipse.swt.widgets.Button;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.CoolBar;
import org.eclipse.swt.widgets.CoolItem;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Event;
import org.eclipse.swt.widgets.FileDialog;
import org.eclipse.swt.widgets.Label;
import org.eclipse.swt.widgets.Listener;
import org.eclipse.swt.widgets.Menu;
import org.eclipse.swt.widgets.MenuItem;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.swt.widgets.Spinner;
import org.eclipse.swt.widgets.Text;


public class MainFrame {
	Shell mShell;
	Menu mMenuBar;
	Menu mRecentMenu;
	MenuItem mFileMenu;
	// The main pane.
	ScrolledComposite mImageHolder;
	ImageCanvas mImage;
	// The footer bar; consists of a couple of labels
	Composite mBottomComposite;
	Label mStatusLeft;
	Label mStatusRight;
//	Label mStatusMid;
	
	// The play button, so we can set it when we stop
	//  playing.
	Button mPlayButton;
	Spinner mFrameSpinner;
		
	// Windows showing views on our log.
	Vector mLogWindows;
	// A vector of log lines.
	Vector mLogLines;
	
	int mLogCounter;
	
	// The text field which holds the number of frames
	// in the current file, so we can update it.
	Text mNumFrames;
	
	// (Persistent) user preferences.
	Prefs mPrefs;
	
	
	/** When we change the status bar, we need to call this
	 *  to resize all the components
	 */
	public void displayStatusBar()
	{
		mStatusLeft.pack(); //mStatusMid.pack();
		mStatusRight.pack();
		
		Point frameSize = mShell.getSize();
		Point sbSize = mBottomComposite.getSize();
		mBottomComposite.setSize(frameSize.x, sbSize.y);
		
		mBottomComposite.layout();
		// mBottomComposite.pack();
	}
	
	/** Display a message in the status bar - used
	 *  for transient messages like the dropper.
	 */
	public void transientInfo(String inString)
	{

		mStatusLeft.setText(inString);
		displayStatusBar();
	}
	
	/** Something for the log (only)
	 *
	 */
	public void log(String inString)
	{
		writeToLog(inString);
	}
	
	/** Something interesting happened; note it.
	 *
	 */
	public void note(String inString)
	{
		// For now, do nothing.
		mStatusLeft.setText(inString);
		writeToLog(inString);
		displayStatusBar();
	}
	
	/** Update the status bar in the bottom right of the
	 *   window.
	 *
	 */
	public void updateStatus()
	{
		StringBuffer buf = new StringBuffer();
		
		buf.append(Integer.toString(mImage.mWidth) + "x" + 
				Integer.toString(mImage.mHeight));
		buf.append(" F:" + Integer.toString(mImage.mFrameNumber) + "/" + 
				Long.toString(mImage.mFrameTotal-1));
		buf.append(" Z:" + Integer.toString(mImage.mZoomNumerator) + ":" + 
				Integer.toString(mImage.mZoomDenominator));
		mStatusRight.setText(buf.toString());
		displayStatusBar();
	}
		
	
	/** Make the macroblock info toolbar
	 *
	 */
	private CoolItem makeMBToolbar(CoolBar inBar)
	{
		Composite toolBar = new Composite(inBar, SWT.FLAT);
		
		toolBar.setLayout(new RowLayout());
		
		// Frame/field button.
		{
			Button frameField = new Button(toolBar,
					SWT.CHECK);
			frameField.setText("Field mode");
			frameField.addSelectionListener(new SelectionAdapter()
					{
					public void widgetSelected(SelectionEvent e)
						{
						  MainFrame.this.mImage.setFieldMode
						  (((Button)e.widget).getSelection());
						}
					});
		}
		{
			Button mbMarkers = new Button(toolBar, SWT.CHECK);
			mbMarkers.setText("MB Markers");
			mbMarkers.addSelectionListener(new SelectionAdapter()
					{
					public void widgetSelected(SelectionEvent e)
					{
						MainFrame.this.mImage.setMBMarkers
						(((Button)e.widget).getSelection());
					}
					});
		}
		

		toolBar.pack();
		Point size = toolBar.getSize();
		CoolItem rv = new CoolItem(inBar, SWT.NONE);
		rv.setControl(toolBar);
		Point preferredSize = rv.computeSize(size.x, size.y);
		rv.setPreferredSize(preferredSize);
		return rv;
	}
	
	/** Make the zoom toolbar
	 *
	 */
	private CoolItem makeZoomToolbar(CoolBar inBar)
	{
		Composite toolBar = new Composite(inBar, SWT.FLAT);

		toolBar.setLayout(new RowLayout());
		
		// Numerator ..
		{
			Spinner spinner = new Spinner(toolBar,
					SWT.BORDER);
			spinner.setMinimum(1);
			spinner.setMaximum(100);
			spinner.setSelection(1);
			spinner.setPageIncrement(1);
			spinner.pack();
			spinner.addModifyListener(new ModifyListener()
					{
						public void modifyText(ModifyEvent e)
						{
							MainFrame.this.mImage.SetZoomNumerator
							(((Spinner)e.widget).getSelection());
						}
					});
					
					
			
			Label sep = new Label(toolBar, SWT.NONE);
			sep.setText(":");
			
			spinner = new Spinner(toolBar, 
					SWT.BORDER);
			spinner.setMinimum(1);
			spinner.setMaximum(100);
			spinner.setSelection(1);
			spinner.setPageIncrement(1);
			spinner.pack();
			spinner.addModifyListener(new ModifyListener()
					{
						public void modifyText(ModifyEvent e)
						{
							MainFrame.this.mImage.SetZoomDenominator
							(((Spinner)e.widget).getSelection());
						}
					});
		}
		toolBar.pack();
		Point size = toolBar.getSize();
		CoolItem rv = new CoolItem(inBar, SWT.NONE);
		rv.setControl(toolBar);
		Point preferredSize = rv.computeSize(size.x, size.y);
		rv.setPreferredSize(preferredSize);
		return rv;
	}
	
	/** Make the frame toolbar 
	 * 
	 */
	private CoolItem makeFrameToolbar(CoolBar inBar)
	{
		Composite toolBar = new Composite(inBar, SWT.FLAT);
		
		toolBar.setLayout(new RowLayout());
	
		// Spinner .
		{
			Spinner spinner = new Spinner(toolBar, SWT.FLAT);
			
			spinner.setMinimum(0);
			spinner.setMaximum(999999);
			spinner.setSelection(0);
			spinner.setPageIncrement(1);
			spinner.pack();
			spinner.addModifyListener(new ModifyListener()
					{
				public void modifyText(ModifyEvent e)
				{
					MainFrame.this.mImage.changeFrameNumber
					(((Spinner)e.widget).getSelection());
				}
					});
			
			mFrameSpinner = spinner;
		
			Label sep = new Label(toolBar, SWT.NONE);
			sep.setText(" of ");
			
			mNumFrames = new Text(toolBar, SWT.NONE);
			mNumFrames.setText("???");
			mNumFrames.setEditable(false);
			
			// Play button.
			mPlayButton = new Button(toolBar, SWT.TOGGLE);
			mPlayButton.setText(">");
			mPlayButton.addListener(
					SWT.Selection,
					new Listener()
					{
						public void handleEvent(Event inEvent)
						{
							MainFrame.this.mImage.setPlayMode
							(((Button)inEvent.widget).getSelection());
						}
					});	
			
		}
		toolBar.pack();
		Point size = toolBar.getSize();
		CoolItem rv = new CoolItem(inBar, SWT.NONE);
		rv.setControl(toolBar);
		Point preferredSize = rv.computeSize(size.x, size.y);
		rv.setPreferredSize(preferredSize);
		return rv;		
	}
	
	/** Make the view menu
	 */
	private MenuItem makeViewMenu(Menu topMenu)
	{
		MenuItem viewMenu = new MenuItem(topMenu, SWT.CASCADE);
		Menu dropDown = new Menu(mShell, SWT.DROP_DOWN);
		viewMenu.setText("View");
		viewMenu.setMenu(dropDown);
		
		{
			MenuItem item = new MenuItem(dropDown, SWT.PUSH);
			item.setText("&Log");
			item.setAccelerator(SWT.CTRL + 'L');
			item.addListener(SWT.Selection,
					new Listener()
					{
					public void handleEvent(Event e)
					{
						openLog();
					}
					});
		}
		return viewMenu;
	};
	
	/** Make the help menu
	 */
	private MenuItem makeHelpMenu(Menu topMenu)
	{
		MenuItem helpItem = new MenuItem(topMenu,
				SWT.CASCADE | SWT.RIGHT);
		helpItem.setText("Help");
		
		Menu helpMenu = new Menu(mShell, SWT.DROP_DOWN);
		helpItem.setMenu(helpMenu);
		
		{
			MenuItem aboutItem = new MenuItem(helpMenu,
					SWT.PUSH);
			aboutItem.setText("About");
			aboutItem.addListener(SWT.Selection,
					new Listener()
					{
					public void handleEvent(Event inEvent)
					{
						MainFrame.this.aboutBox();
					}});
		}
		
		return  helpItem;
	}
	
	/** Make the recent items menu
	 * 
	 */
	private MenuItem makeRecentMenu(Menu topMenu)
	{
		MenuItem recentMenu = new MenuItem(topMenu, 
					SWT.CASCADE);
		mRecentMenu = new Menu(mShell, SWT.DROP_DOWN);
		recentMenu.setText("Recent");
		recentMenu.setMenu(mRecentMenu);
		
		mRecentMenu.addListener(SWT.Show,
				new Listener()
				{
					public void handleEvent(Event event)
					{
						fillRecentMenu();
					}
				});
		
		return recentMenu;
	};
		
	/** Make the file menu.
	 * 
	 * @param topMenu
	 * @return
	 */
	private MenuItem makeFileMenu(Menu topMenu)
	{
		MenuItem fileMenu = new MenuItem(topMenu, SWT.CASCADE);
		Menu dropDown = new Menu(mShell, SWT.DROP_DOWN);
		fileMenu.setText("File");
		fileMenu.setMenu(dropDown);
		
		// Open
		{
			MenuItem item = new MenuItem(dropDown, SWT.PUSH);
			item.setText("&Open");
			item.setAccelerator(SWT.CTRL + 'O');
			item.addListener(SWT.Selection,
					new Listener() 
					{
					public void handleEvent(Event e)
					{
						MainFrame.this.openFile(null);
					}
					});
		}

		// Set parameters.
		{
			MenuItem item = new MenuItem(dropDown, SWT.PUSH);
			item.setText("&Dimensions");
			item.setAccelerator(SWT.CTRL + 'D');
			item.addListener(SWT.Selection,
					new Listener()
					{
				public void handleEvent(Event e)
				{
					MainFrame.this.setFormat
					(mPrefs.getDimensions
						(mImage.mFilename));
				}
					});
		}			
	
				
		// Separator
		{
			new MenuItem(dropDown, SWT.SEPARATOR);
		}
		
		// Clear log.
		{
			MenuItem item = new MenuItem(dropDown, SWT.PUSH);
			item.setText("&Clear logs");
			item.setAccelerator(SWT.CTRL + 'C');
			item.addListener(SWT.Selection,
					new Listener()
					{
					public void handleEvent(Event e)
					{
						MainFrame.this.clearLog();
					}
					});
		}
			
		
		// Quit
		{
			MenuItem item = new MenuItem(dropDown, SWT.PUSH);
			item.setText("E&xit");
			item.setAccelerator(SWT.CTRL + 'X');
			item.addListener(SWT.Selection,
					new Listener() {
					public void handleEvent(Event e)
					{
						MainFrame.this.quit();
					}
			});
		}
		
		return fileMenu;
	}
		
	/** Opens a log window, and adds it to the
	 *  vector of log windows we maintain. 
	 *  
	 *  The idea here is that you can open many log windows..
	 *
	 */
	void openLog()
	{
		LogWindow newLog = new LogWindow(mShell.getDisplay(),
										mLogLines, mLogCounter);
		++mLogCounter;
		mLogWindows.addElement(newLog);		
	}
	
	private void clearLog()
	{
		boolean needTidy = false;
		
		// Clear the logs.
		mLogLines = new Vector();
		for (int i=0;i<mLogWindows.size();++i)
		{
			LogWindow cur = (LogWindow)mLogWindows.elementAt(i);
			
			if (!cur.disposed())
			{
				cur.clearLog();
			}
			else
			{
				needTidy = true;
			}
		}
		
		
		if (needTidy)
		{
			cleanLogVector();
		}
	}
	
	private void aboutBox()
	{
		AboutBox theBox = new AboutBox(mShell);
		
		theBox.show();
	}
	
	/** Write to the log. Private so external routines go through
	 *   one of the public wombats above.
	 */
	private void writeToLog(String logEntry)
	{
		boolean needsCleaning = false;
		boolean any = false;
		
		mLogLines.addElement(logEntry);
		// Now tell all our loggers about it, removing those
		//  that don't exist in the process. 
		for (int i=0;i<mLogWindows.size();++i)
		{
			LogWindow cur = (LogWindow)mLogWindows.elementAt(i);
			if (cur.disposed())
			{
				needsCleaning = true;
			}
			else
			{
				any = true;
				cur.appendToLog(logEntry);
			}
		}
		
		if (needsCleaning)
		{
			cleanLogVector();
		}
	//	if (!any)
//		{
//			openLog();
//		}
		
	}
	
	void cleanLogVector()
	{
		Vector newLogs;
		
		newLogs = new Vector();
		for (int i=0;i<mLogWindows.size();++i)
		{
			LogWindow cur = (LogWindow)mLogWindows.elementAt(i);
			if (!cur.disposed())
			{
				newLogs.addElement(cur);
			}
		}
		mLogWindows = newLogs;
	}
	
	
	void quit()
	{
		//System.out.println("Wombats!");
		mPrefs.flush(this.mShell);
    	mImage.prepareForExit();
    	// this.mShell.close();
		YUV2.Exit();
	}
	
	void fillRecentMenu()
	{
		MenuItem [] menuItems = mRecentMenu.getItems ();
	
		for (int i=0;i < menuItems.length; 
			++i)
		{
			menuItems[i].dispose();
		}
		
		Vector theVec = mPrefs.getRecentFiles();
		
		for (int i=0;i < theVec.size(); ++i)
		{
			String fileName = (String)theVec.elementAt(i);
			MenuItem theItem = new MenuItem(mRecentMenu, 
						SWT.PUSH);
			theItem.setText(fileName);
			theItem.addListener(SWT.Selection,
					new Listener()
					{
		
				public void handleEvent(Event inEvent)
				{
					MainFrame.this.openFile
					(((MenuItem)inEvent.widget).getText());
				}
					});
					
		}
	}

	MainFrame(Display d, String inInitialDimensions, String inInitialFile)
		{
			mLogWindows = new Vector();
			mLogLines = new Vector();
			mLogCounter = 0;
						
			mShell = new Shell(d);
			mPrefs = new Prefs(mShell);
		
			
			mImageHolder = new ScrolledComposite(mShell, 
					SWT.H_SCROLL | SWT.V_SCROLL | SWT.BORDER);
			mImage = new ImageCanvas(mImageHolder, this,
					mShell,
					SWT.NONE);
			// For some odd reason, we have to do a ..
			mImageHolder.setContent(mImage);
			
			// Make sure we fire up at a vaguely sensible
			//  size.
			mImage.setSize(768, 576); 
			mImage.pack();
			mImageHolder.setSize(mImageHolder.computeSize(SWT.DEFAULT,
					SWT.DEFAULT));
			//mImageHolder.setExpandHorizontal(true);
			//mImageHolder.setExpandVertical(true);
			
			// Make a fancy title n stuff..
			mShell.setText("YUV2");
		
			
			// Add some menus ..
			mMenuBar = new Menu(mShell, SWT.BAR);
			mShell.setMenuBar(mMenuBar);
			mFileMenu = makeFileMenu(mMenuBar);
			makeViewMenu(mMenuBar);
			makeRecentMenu(mMenuBar);
			makeHelpMenu(mMenuBar);
			
			// Register a close handler to quit the program.
			mShell.addListener(SWT.Close,
					new Listener()
					{
					public void handleEvent(Event inEvent)
					{
						MainFrame.this.quit();
					}
					});
					
			
			
			FormLayout topLevelLayout = new FormLayout();
			mShell.setLayout(topLevelLayout);
			
			// Arrange a window with a toolbar at the top, infobar at the
			// bottom, and a viewing window in the middle.
			CoolBar coolBar = new CoolBar(mShell, SWT.BORDER);
			
			
			makeFrameToolbar(coolBar);
			makeZoomToolbar(coolBar);
			makeMBToolbar(coolBar);
			
			coolBar.setLayoutData(Utils.makeFormData
					(new FormAttachment(0), 
							new FormAttachment(100),
							new FormAttachment(0), null));
			coolBar.addListener(SWT.Resize, 
					new Listener() 
					{
					public void handleEvent(Event ev)
					{
						mShell.layout();
					}
					});
		
			// Footer bar.
			mBottomComposite = 
				new Composite(mShell, SWT.NONE);
	
			{
				FormLayout former = new FormLayout();

				mBottomComposite.setLayout(former);
			}
				
			mImageHolder.setLayoutData(Utils.makeFormData(
				new FormAttachment(0),
				new FormAttachment(100),
				new FormAttachment(coolBar),
				new FormAttachment(mBottomComposite)));	
			
			mBottomComposite.setLayoutData(Utils.makeFormData(
				new FormAttachment(0),
				new FormAttachment(100),
				null, // No circularity .. 
				new FormAttachment(100)));
			
		    mStatusLeft = new Label(mBottomComposite, SWT.LEFT);	
			//mStatusMid = new Label(mBottomComposite, SWT.LEFT);

			mStatusRight = new Label(mBottomComposite,  SWT.RIGHT);
				
			mStatusLeft.setLayoutData(Utils.makeFormData(
					new FormAttachment(0),
					null,
					new FormAttachment(0),
					null));
			mStatusLeft.setText("idle");
			
			//mStatusMid.setLayoutData(Utils.makeFormData(
			//		null,
		//			null,
		//			new FormAttachment(0),
		//			null));
	//		mStatusMid.setText("");
			
			mStatusRight.setLayoutData(Utils.makeFormData(
					new FormAttachment(mStatusLeft),
					new FormAttachment(100),
					new FormAttachment(0),
					null));
			
			mStatusLeft.pack(); mStatusRight.pack();
			mBottomComposite.pack();
			
			mShell.pack();
			mImage.pack();
			mImageHolder.pack();
			coolBar.pack();
			mShell.setSize(700, 500);

			initialLoadFile(inInitialFile, inInitialDimensions);
		}

	boolean isDisposed() { return mShell.isDisposed(); }
	
    void open()
	{
		mShell.open();
	}
	
    // Called to update the number of frames in the file when
    //  ImageCanvas knows this information.
    public void setNumFrames(long numFrames)
    {
    	mNumFrames.setText(Long.toString(numFrames-1));
    	mNumFrames.pack();
    }
    
    
    
    
    // Action routines.
    public void initialLoadFile(String inFile, String inDimensions)
    {
    	Prefs.FileDimensionInfo fdi = null;
    	
    	if (inDimensions != null)
    	{    		
    		// Parse out the dimensions and add to the recent files
    		// list.
    		boolean parse_ok;
    		Point result = new Point(0,0);
    		
    		parse_ok = Utils.parseDimensions(inDimensions, result);
    		if (parse_ok) 
    		{
    			fdi = new Prefs.FileDimensionInfo(inFile, result.y, result.x, 
    						Utils.FORMAT_YUV);
   	    		
    		}
    	}
    	
    	if (inFile != null)
    	{
    		boolean openedFile = false;
    		
    		if (mImage.openFile(inFile))
    		{
    			openedFile = true;
    			mPrefs.registerFileOpen(inFile);
    		}
    		
    		if (inDimensions == null)
    		{

    			fdi  = mPrefs.getDimensions(inFile);
    		}
    		
    		if (fdi.mFilename == null)
    		{    		
    			boolean doSave = displayFormatDialog(fdi);
    			
    		
    			if (doSave)
    			{
    				mPrefs.setDimensions(fdi);
    			}
    		}
    	
    		if (openedFile)
    		{
    			// We opened a file. Save the current dimensions back
    			// with this file.
    			fdi.mFilename = inFile;
    			mPrefs.setDimensions(fdi);
    		}
    	
    		
    		mImage.changeFormat(fdi.mWidth, 
    				fdi.mHeight, 
    				fdi.mFormat);
    	}
    }	
    
    public void openFile(String inFile)
    {
    	String fileName;
    	
    	if (inFile == null)
    	{
    		FileDialog theDialog = new FileDialog(mShell, 
    				SWT.OPEN);
  
    		theDialog.setFilterNames(new String[]{"YUV files", "All Files"});
    		theDialog.setFilterExtensions
    			(new String[]{"*.yuv","*.*"});
    		theDialog.setFilterPath("c:\\"); 
    		theDialog.setFileName("viddec.yuv");
    		fileName = theDialog.open();
    	}
    	else
    	{
    		fileName = inFile;
    	}

    	if (fileName == null)
    	{
    		// Someone clicked cancel on a dialog box.
    		return;
    	}
    	
    	if (mImage.openFile(fileName))
    		{
    			boolean doSave = false;
    			
    		// Stash the file in the recent files list.
    			mPrefs.registerFileOpen(fileName);
    			
    			// Do we have dimensions for this file?
    			Prefs.FileDimensionInfo fdInfo = 
    				 mPrefs.getDimensions(fileName);
    		
    		
    			if (fdInfo.mFilename == null)
    			{
    				// Need to ask.
    				doSave = displayFormatDialog(fdInfo);
    			}
    			else
    			{
    				doSave = true;
    			}
    			
    			// Unconditionally save, so future
    			// invocations work propely and we maintain
    			//  the order of file opens.
    			if (doSave) 
    			{
    				// If we had the defaults before, specialise them for
    				//  this file.
    				if (fdInfo.mFilename == null)
    				{
    					fdInfo.mFilename = fileName;
    				}
    				
    				// Tell the image to update itself. 
    	    		mImage.changeFormat(fdInfo.mWidth, 
    	    				fdInfo.mHeight, 
    	    				fdInfo.mFormat);
    	    		
    				mPrefs.setDimensions(fdInfo);
    			}
    		}
    }
    
    public boolean displayFormatDialog(Prefs.FileDimensionInfo inInfo)
    {
    	boolean dialog_ok;
    	
    	FormatDialog theDialog =
    			new FormatDialog(mShell, 
    					inInfo.mWidth, inInfo.mHeight,
    					inInfo.mFormat);
    	dialog_ok = theDialog.show();
    	 
    	if (dialog_ok)
    	{
    		inInfo.mWidth = theDialog.mWidth;
    		inInfo.mHeight = theDialog.mHeight;
    		inInfo.mFormat = theDialog.mFormat;
    	}
    	return dialog_ok;
    }
    
    /** Set the format and dimensions of the current file.
     */
    public void setFormat(Prefs.FileDimensionInfo fdInfo)
    {	
    	boolean dialog_ok = displayFormatDialog(fdInfo);
    	if (dialog_ok)
    	{
    		// Tell the image to update itself. 
    		mImage.changeFormat(fdInfo.mWidth, 
    				fdInfo.mHeight, 
    				fdInfo.mFormat);
    		
			mPrefs.setDimensions(fdInfo);   
    	}
    }
    
    /** We just advanced a frame.
     */
    public void playAdvance()
    {
    	int frameToSet = mImage.mFrameNumber;
    	
    	++frameToSet;
    	mFrameSpinner.setSelection(frameToSet);
    	mImage.changeFrameNumber(frameToSet); 	
    	// mAdvancePending is marked by ImageCanvas, once it's
    	//  actually done painting.
    }
    
    /** We stopped playing; pop out the play button
     */
    public void playStopped()
    {
    	mImage.mPlayMode = false;
    		// The test here is necessary, because we get
    	// a spurious event when ImageCanvas's player starts up.
    	if (mPlayButton != null)
    	{
    		mPlayButton.setSelection(false);
    	}
    	synchronized (mImage)
    	{
    		mImage.mAdvancePending = false;
    	}
    }
    
   

    
}
