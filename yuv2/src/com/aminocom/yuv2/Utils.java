/* 
 * General utilities.
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
import org.eclipse.swt.layout.FormData;
import org.eclipse.swt.widgets.MessageBox;
import org.eclipse.swt.widgets.Shell;



public class Utils {
	public static final int FORMAT_YUV = 0;
	
	public static String formatToString(int inFormat)
	{
		// We only know about one format so far.
		return "YUV 4:2:0";
	}

	/** Try to parse a string as a set of dimensions. 
	*
	* @return true on success, false on failure.
	*/
	public static boolean parseDimensions(String inString, Point rv)
	{
		// We're OK. Parse out the combo box.

		boolean dimensionsOK = false;
		
		// Bit before the 'x'.
		int pos = inString.indexOf('x');
		if (pos >= 0)
		{
			try
			{
				rv.x = Integer.parseInt(inString.substring(0, pos));
				rv.y = Integer.parseInt(inString.substring(pos+1));
				dimensionsOK = true;
			}
			catch (NumberFormatException nfe)
			{
				// Do nothing - we're about to issue an error
				// anyway.
			};
		}
	
		return dimensionsOK;
	}
	/** Make form data from attachments
	 * 
	 */
	public static FormData makeFormData(FormAttachment inLeft,
			FormAttachment inRight,
			FormAttachment inTop,
			FormAttachment inBottom)
	{
		FormData rv = new FormData();
		rv.left = inLeft; rv.right = inRight;
		rv.top = inTop; rv.bottom= inBottom;
		return rv;
	}
	public static void showMessageBox(Shell inShell, String s)
	{
		MessageBox mb = new MessageBox(inShell, SWT.OK);
		mb.setMessage(s);
		mb.open();
	}
	
}
