/* 
 * Code for the About box.
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
import org.eclipse.swt.custom.StyledText;
import org.eclipse.swt.layout.FormAttachment;
import org.eclipse.swt.layout.FormLayout;
import org.eclipse.swt.layout.RowLayout;
import org.eclipse.swt.widgets.Button;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Event;
import org.eclipse.swt.widgets.Label;
import org.eclipse.swt.widgets.Listener;
import org.eclipse.swt.widgets.Shell;

public class AboutBox {
   Shell mShell;
   
   public static final String sCommentText =
   "Pre-release version. Please report bugs to \n" + 
	 " <rrw@sj.co.uk>";
   
  public AboutBox(Shell inParent)
  {
	  mShell = new Shell(inParent);
	  mShell.setLayout(new FormLayout());
	  mShell.setText("About YUV2");
	  
	  Composite topPanel = new Composite(mShell, SWT.NONE);
	  Composite bottomPanel = new Composite(mShell,
			  SWT.NONE);
	  
	  Label about = new Label(topPanel, SWT.NONE);
	  about.setText("YUV2 0.3 (c) SJ Consulting 2006");
	  
	  StyledText mainText = new StyledText(topPanel,
			  SWT.BORDER | SWT.H_SCROLL | SWT.V_SCROLL);
	  mainText.setText(sCommentText);
	  
	  {
		  RowLayout rl = new RowLayout();
		  rl.fill = true;
		  rl.type = SWT.VERTICAL;
		  topPanel.setLayout(rl);
	  }
	  
	  {
		  RowLayout rl = new RowLayout();
		  bottomPanel.setLayout(rl);
	  }
	  
	  Button ok = new Button(bottomPanel, SWT.NONE);
	  ok.setText("Dismiss");
	  ok.addListener(SWT.Selection,
			  new Listener()
			  {
		  public void handleEvent(Event ev)
		  {
			  AboutBox.this.close();
		  }
			  });
	  
	  topPanel.setLayoutData(Utils.makeFormData
			  	(new FormAttachment(0),
			  	 new FormAttachment(100),
			  	 new FormAttachment(0),
			  	 new FormAttachment(bottomPanel)));
	  
	  bottomPanel.setLayoutData(Utils.makeFormData
			  (new FormAttachment(0),
			   new FormAttachment(100),
			   null,
			   new FormAttachment(100)));
	  mShell.pack();
  }
   public void show()
   {
	   mShell.open();
	   while (!mShell.isDisposed())
			{
			 if (!mShell.getDisplay().readAndDispatch())
			 {
				 mShell.getDisplay().sleep();
			 }
			}
   }
   
   public void close()
   {
	   mShell.close();
   }
   
   public boolean isDisposed()
   {
	   return mShell.isDisposed();
   }
	
}
