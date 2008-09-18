/* 
 * A log window.
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
import org.eclipse.swt.custom.StyledText;
import org.eclipse.swt.events.SelectionAdapter;
import org.eclipse.swt.events.SelectionEvent;
import org.eclipse.swt.graphics.Font;
import org.eclipse.swt.graphics.FontData;
import org.eclipse.swt.layout.FillLayout;
import org.eclipse.swt.layout.GridData;
import org.eclipse.swt.layout.GridLayout;
import org.eclipse.swt.layout.RowLayout;
import org.eclipse.swt.widgets.Button;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Shell;

/*** This class wraps a shell that displays a log window, used
 *   for persistent logging. The window is associated with a
 *   MainFrame, and exists until closed.
 *   
 * @author rrw
 *
 */
public class LogWindow {
	Shell mShell;
	ScrolledComposite mScrollArea;
	StyledText mData;
	boolean mDisposed;
	
	LogWindow(Display inDisplay, Vector logLines, int nr)
	{
		mShell = new Shell(inDisplay);
		mShell.setText("Log " + Integer.toString(nr));
		mDisposed = false;
		{
			GridLayout rows = new GridLayout();
			rows.numColumns = 1;
			
			mShell.setLayout(rows);
		}
		
	//	mScrollArea = new ScrolledComposite(mShell, 
	//				SWT.BORDER | SWT.H_SCROLL | SWT.V_SCROLL);
		mData = new StyledText(mShell, SWT.BORDER | SWT.H_SCROLL | SWT.V_SCROLL);
		mData.setEditable(false);
//		mScrollArea.setLayout(new FillLayout());
		// Attempt to set a fixed pitch font.
		mData.setFont(new Font(null, new FontData("Courier", 8, SWT.NORMAL)));
		{
			GridData scrollData = new GridData();
			
			scrollData.grabExcessHorizontalSpace = true;
		scrollData.grabExcessVerticalSpace = true;
			scrollData.verticalAlignment = SWT.FILL;
			scrollData.horizontalAlignment = SWT.FILL;
			mData.setLayoutData(scrollData);
		}
		
	//	mScrollArea.setContent(mData);
	//	mScrollArea.setExpandHorizontal(true);
	//	mScrollArea.setExpandVertical(true);
	//	mScrollArea.setAlwaysShowScrollBars(true);
	//	mScrollArea.pack();

		mData.pack();
		{
			Composite buttonBar = new Composite(mShell, 0);
			buttonBar.setLayout(new RowLayout());
			
			Button ok = new Button(buttonBar, 0);
			ok.setText("Dismiss");
			ok.addSelectionListener(new SelectionAdapter()
					{
				public void widgetSelected(SelectionEvent e)
				{
					LogWindow.this.close();
				}
					});

			Button clear = new Button(buttonBar, 0);
			clear.setText("Clear");
			clear.addSelectionListener(new SelectionAdapter()
					{
					public void widgetSelected(SelectionEvent e)
					{
						LogWindow.this.mData.setText("");
					}
					});
			
			
			
			{
				GridData barData = new GridData(GridData.HORIZONTAL_ALIGN_CENTER);
				barData.verticalAlignment = SWT.CENTER;
				barData.verticalAlignment = SWT.CENTER;
				barData.grabExcessHorizontalSpace = false;
				barData.grabExcessVerticalSpace = false;
				buttonBar.setLayoutData(barData);
				buttonBar.pack();
			}
		}
			
		loadData(logLines);
		mShell.pack();
		mShell.setSize(600, 200);
		mShell.open();
	}
	
	/** Clear the log and load data into it. If you 
	 *   set data to null, nothing will be loaded.
	 */
	void loadData(Vector inLines)
	{
		// Clear the content of the styled text.
		mData.setText("");
		
		if (inLines != null)
		{
			for (int i=0;i<inLines.size();++i)
			{
				mData.append((String)inLines.elementAt(i));
				mData.append("\n");
			}
		}
		// Move to the end of the text.
		mData.setSelection(mData.getCharCount()-1);
		
		mData.redraw();
	}
	
	/** The log has been cleared - clear our copy of i
	 */
	void clearLog()
	{
	 loadData(null);
	}
	
	/** The log has been written to - update our copy.
	 *
	 */
	void appendToLog(String s)
	{
		mData.append(s);
		mData.append("\n");

		// Purely for the scroll side-effect ...
		mData.setSelection(mData.getCharCount()-1);
		mData.redraw();
	}
	
	public boolean disposed() 
	{
		return mDisposed; 
	}
	
	void close()
	{
		mDisposed = true;
		mShell.close();
	}	
	
}
