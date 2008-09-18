/* 
 * The image format dialogue.
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

import org.eclipse.swt.SWT;
import org.eclipse.swt.graphics.Point;
import org.eclipse.swt.layout.FormAttachment;
import org.eclipse.swt.layout.FormLayout;
import org.eclipse.swt.layout.RowLayout;
import org.eclipse.swt.widgets.Button;
import org.eclipse.swt.widgets.Combo;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Event;
import org.eclipse.swt.widgets.Group;
import org.eclipse.swt.widgets.Label;
import org.eclipse.swt.widgets.Listener;
import org.eclipse.swt.widgets.Shell;

/** This class wraps the format and type dialog.
 *  Create one, then call show() on it to show the dialog
 *   and modify the relevant values.
 * 
 * @author rrw
 *
 */
public class FormatDialog {
	Shell mDialogShell;
	Shell mMessageShell;
	int mWidth;
	int mHeight;
	int mFormat;
	boolean mIsOK;
	Combo mDimensionCombo;
	
	public final String[] sStandardDimensions = 
	  { "480x480", "720x480" };
	
	FormatDialog(Shell parentShell, 
				int inWidth, 
				int inHeight,
				int inFormat)
	{
		mMessageShell = parentShell;
		mDialogShell = new Shell(parentShell, 
					SWT.DIALOG_TRIM |
					SWT.APPLICATION_MODAL);
     mDialogShell.setText("Select image dimensions");
     mDialogShell.setSize(300, 200);
     mWidth = inWidth; mHeight = inHeight; mFormat = inFormat;	
     
     Group fileSize = new Group(mDialogShell, SWT.NONE);
     fileSize.setText("Dimensions");
    	
     // Image dimensions thingy ..
     {
    		Label aLabel;
    		Composite hRow = new Composite(fileSize, SWT.NONE);
    		
    		fileSize.setLayout(new FormLayout());
    		hRow.setLayout(new RowLayout());
    							
    		hRow.setLayoutData(Utils.makeFormData
				(new FormAttachment(20),
						new FormAttachment(100),
						new FormAttachment(20),
						new FormAttachment(100)));
		
    		aLabel = new Label(hRow, SWT.NONE);
    		aLabel.setText("Image Size:");
    		mDimensionCombo = new Combo(hRow, SWT.NONE);
    		mDimensionCombo.setItems(sStandardDimensions);	
    		if (mWidth > 0 && mHeight > 0)
    		{
    			mDimensionCombo.add
    				(Integer.toString(mWidth) + "x" +
    						Integer.toString(mHeight), 0);
    			mDimensionCombo.select
    				(0);
    		}
    	}

     Group fileFormat = new Group(mDialogShell, SWT.NONE);
 	fileFormat.setText("File Type");
 	
    	// Image format.
    	{
    		Composite hCol = new Composite(fileFormat, SWT.NONE);
    		RowLayout theLayout = new RowLayout();
    		
    		theLayout.type = SWT.VERTICAL;
    		hCol.setLayout(theLayout);
    		
    		{
    			Button aButton = new Button(hCol, SWT.RADIO | SWT.RIGHT);
    			aButton.setText("YUV");
    			aButton.setSelection(mFormat == 0 ? true : 
    				false);
    		}
    		
    		fileFormat.setLayout(new FormLayout());
    		hCol.setLayoutData(Utils.makeFormData
    				(new FormAttachment(20),
    						new FormAttachment(100),
    						new FormAttachment(20),
    						new FormAttachment(100)));
    	}
    	
    	
    	
    	Composite buttons = new Composite(mDialogShell, 
    				SWT.NONE);
    	buttons.setLayout(new RowLayout());
    	
    	Button ok = new Button(buttons, SWT.PUSH);
    	ok.setText("Ok");
    	mDialogShell.setDefaultButton(ok);
    	ok.addListener(SWT.Selection, new Listener() 
    			{ 
    			public void handleEvent(Event event)
    			{
    				boolean parsedOK = parseDialog();
    				
    				if (parsedOK)
    					{
    						FormatDialog.this.mIsOK = true; 
    						FormatDialog.this.mDialogShell.close();
    					}
    			} 
    			});
    	
    	Button cancel = new Button(buttons, SWT.PUSH);
    	cancel.setText("Cancel");
    	cancel.addListener(SWT.Selection, new Listener()
    			{ 
    		public void handleEvent(Event event)
    		{
    			FormatDialog.this.mIsOK = false;
    			FormatDialog.this.mDialogShell.close();
    		}
    			});
    	
    	
    	mDialogShell.setLayout(new FormLayout());
    	fileSize.setLayoutData(Utils.makeFormData
    			(new FormAttachment(0),
    		     new FormAttachment(fileFormat),
    		     new FormAttachment(0),
    		     new FormAttachment(buttons)));
    	
    	fileFormat.setLayoutData(Utils.makeFormData
    			(null,
    					new FormAttachment(100),
    					new FormAttachment(0),
    					new FormAttachment(buttons)));
    	buttons.setLayoutData(Utils.makeFormData
    			(new FormAttachment(0),
    			 new FormAttachment(100),
    			 null,
    			 new FormAttachment(100)));
    	    	
    	// Add the size and type to the group 
    	
    	
    	mDialogShell.open(); 	
	}
	
	/** Try to parse the dialog box. If we succeed,
	 *   return true. If we don't, pop up a message
	 *   box and return false.
	 */
	boolean parseDialog()
	{
		boolean parsedOK;
		String dimensionString = mDimensionCombo.getText();
		String s = mDimensionCombo.getText();
		Point results = new Point(mWidth,mHeight);
		parsedOK = Utils.parseDimensions(s, results);
		
			if (!parsedOK)
			{
				Utils.showMessageBox(mMessageShell,
							"Invalid dimensions - " + dimensionString + 
							" - please try again");
			}
			else
			{
				mWidth = results.x; mHeight = results.y;
			}
		return parsedOK;
	}
	
	
	
	boolean show()
	{
	 mDialogShell.open();
	 while (!mDialogShell.isDisposed())
		{
		 if (!mDialogShell.getDisplay().readAndDispatch())
		 {
			 mDialogShell.getDisplay().sleep();
		 }
		}
	 
	 return mIsOK;		
	}

}

/* End File */
