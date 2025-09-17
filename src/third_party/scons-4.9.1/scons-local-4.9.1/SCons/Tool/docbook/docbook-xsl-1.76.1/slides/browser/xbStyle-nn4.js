/*
 * xbStyle-nn4.js
 * $Revision: 1.2 $ $Date: 2003/02/07 16:04:22 $
 */

/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
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
 * The Original Code is Netscape code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2001
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s): Bob Clary <bclary@netscape.com>
 *
 * ***** END LICENSE BLOCK ***** */

/////////////////////////////////////////////////////////////
// xbStyle.getClip()

function nsxbStyleGetClip()
{
  var clip = this.styleObj.clip;
  var rect = new xbClipRect(clip.top, clip.right, clip.bottom, clip.left);
  return rect.toString();
}

/////////////////////////////////////////////////////////////
// xbStyle.setClip()

function nsxbStyleSetClip(sClipString)
{
  var rect          = new xbClipRect(sClipString);
  this.styleObj.clip.top    = rect.top;
  this.styleObj.clip.right  = rect.right;
  this.styleObj.clip.bottom  = rect.bottom;
  this.styleObj.clip.left    = rect.left;
}

/////////////////////////////////////////////////////////////
// xbStyle.getClipTop()

function nsxbStyleGetClipTop()
{
  return this.styleObj.clip.top;
}

/////////////////////////////////////////////////////////////
// xbStyle.setClipTop()

function nsxbStyleSetClipTop(top)
{
  return this.styleObj.clip.top = top;
}

/////////////////////////////////////////////////////////////
// xbStyle.getClipRight()

function nsxbStyleGetClipRight()
{
  return this.styleObj.clip.right;
}

/////////////////////////////////////////////////////////////
// xbStyle.setClipRight()

function nsxbStyleSetClipRight(right)
{
  return this.styleObj.clip.right = right;
}

/////////////////////////////////////////////////////////////
// xbStyle.getClipBottom()

function nsxbStyleGetClipBottom()
{
  return this.styleObj.clip.bottom;
}

/////////////////////////////////////////////////////////////
// xbStyle.setClipBottom()

function nsxbStyleSetClipBottom(bottom)
{
  return this.styleObj.clip.bottom = bottom;
}

/////////////////////////////////////////////////////////////
// xbStyle.getClipLeft()

function nsxbStyleGetClipLeft()
{
  return this.styleObj.clip.left;
}

/////////////////////////////////////////////////////////////
// xbStyle.setClipLeft()

function nsxbStyleSetClipLeft(left)
{
  return this.styleObj.clip.left = left;
}

/////////////////////////////////////////////////////////////
// xbStyle.getClipWidth()

function nsxbStyleGetClipWidth()
{
  return this.styleObj.clip.width;
}

/////////////////////////////////////////////////////////////
// xbStyle.setClipWidth()

function nsxbStyleSetClipWidth(width)
{
  return this.styleObj.clip.width = width;
}

/////////////////////////////////////////////////////////////
// xbStyle.getClipHeight()

function nsxbStyleGetClipHeight()
{
  return this.styleObj.clip.height;
}

/////////////////////////////////////////////////////////////
// xbStyle.setClipHeight()

function nsxbStyleSetClipHeight(height)
{
  return this.styleObj.clip.height = height;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getLeft()

function nsxbStyleGetLeft()
{
  return this.styleObj.left;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setLeft()

function nsxbStyleSetLeft(left)
{
  this.styleObj.left = left;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getTop()

function nsxbStyleGetTop()
{
  return this.styleObj.top;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setTop()

function nsxbStyleSetTop(top)
{
  this.styleObj.top = top;
}


/////////////////////////////////////////////////////////////////////////////
// xbStyle.getPageX()

function nsxbStyleGetPageX()
{
  return this.styleObj.pageX;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setPageX()

function nsxbStyleSetPageX(x)
{
  this.styleObj.x = this.styleObj.x  + x - this.styleObj.pageX;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getPageY()


function nsxbStyleGetPageY()
{
  return this.styleObj.pageY;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setPageY()

function nsxbStyleSetPageY(y)
{
  this.styleObj.y = this.styleObj.y  + y - this.styleObj.pageY;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getHeight()

function nsxbStyleGetHeight()
{
  //if (this.styleObj.document && this.styleObj.document.height)
  //  return this.styleObj.document.height;
    
  return this.styleObj.clip.height;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setHeight()

function nsxbStyleSetHeight(height)
{
  this.styleObj.clip.height = height;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getWidth()

function nsxbStyleGetWidth()
{
  //if (this.styleObj.document && this.styleObj.document.width)
  //  return this.styleObj.document.width;
    
  return this.styleObj.clip.width;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setWidth()

// netscape will not dynamically change the width of a 
// layer. It will only happen upon a refresh.
function nsxbStyleSetWidth(width)
{
  this.styleObj.clip.width = width;
}

/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getVisibility()

function nsxbStyleGetVisibility()
{
  switch(this.styleObj.visibility)
  {
  case 'hide':
    return 'hidden';
  case 'show':
    return 'visible';
  }
  return '';
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setVisibility()

function nsxbStyleSetVisibility(visibility)
{
  switch(visibility)
  {
  case 'hidden':
    visibility = 'hide';
    break;
  case 'visible':
    visibility = 'show';
    break;
  case 'inherit':
    break;
  default:
    visibility = 'show';
    break;
  }
  this.styleObj.visibility = visibility;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getzIndex()

function nsxbStyleGetzIndex()
{
  return this.styleObj.zIndex;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setzIndex()

function nsxbStyleSetzIndex(zIndex)
{
  this.styleObj.zIndex = zIndex;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getBackgroundColor()

function nsxbStyleGetBackgroundColor()
{
  return this.styleObj.bgColor;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setBackgroundColor()

function nsxbStyleSetBackgroundColor(color)
{
  if (color)
  {
    this.styleObj.bgColor = color;
    this.object.document.bgColor = color;
    this.resizeTo(this.getWidth(), this.getHeight());
  }
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.getColor()

function nsxbStyleGetColor()
{
  return '#ffffff';
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setColor()

function nsxbStyleSetColor(color)
{
  this.object.document.fgColor = color;
}


/////////////////////////////////////////////////////////////////////////////
// xbStyle.moveAbove()

function xbStyleMoveAbove(cont)
{
  this.setzIndex(cont.getzIndex()+1);
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.moveBelow()

function xbStyleMoveBelow(cont)
{
  var zindex = cont.getzIndex() - 1;
            
  this.setzIndex(zindex);
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.moveBy()

function xbStyleMoveBy(deltaX, deltaY)
{
  this.moveTo(this.getLeft() + deltaX, this.getTop() + deltaY);
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.moveTo()

function xbStyleMoveTo(x, y)
{
  this.setLeft(x);
  this.setTop(y);
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.moveToAbsolute()

function xbStyleMoveToAbsolute(x, y)
{
  this.setPageX(x);
  this.setPageY(y);
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.resizeBy()

function xbStyleResizeBy(deltaX, deltaY)
{
  this.setWidth( this.getWidth() + deltaX );
  this.setHeight( this.getHeight() + deltaY );
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.resizeTo()

function xbStyleResizeTo(x, y)
{
  this.setWidth(x);
  this.setHeight(y);
}

////////////////////////////////////////////////////////////////////////
// Navigator 4.x resizing...

function nsxbStyleOnresize()
{
    if (saveInnerWidth != xbGetWindowWidth() || saveInnerHeight != xbGetWindowHeight())
    location.reload();

  return false;
}

/////////////////////////////////////////////////////////////////////////////
// xbStyle.setInnerHTML()

function nsxbSetInnerHTML(str)
{
  this.object.document.open('text/html');
  this.object.document.write(str);
  this.object.document.close();
}

xbStyle.prototype.getClip            = nsxbStyleGetClip;
xbStyle.prototype.setClip            = nsxbStyleSetClip;  
xbStyle.prototype.getClipTop         = nsxbStyleGetClipTop;
xbStyle.prototype.setClipTop         = nsxbStyleSetClipTop;  
xbStyle.prototype.getClipRight       = nsxbStyleGetClipRight;
xbStyle.prototype.setClipRight       = nsxbStyleSetClipRight;  
xbStyle.prototype.getClipBottom      = nsxbStyleGetClipBottom;
xbStyle.prototype.setClipBottom      = nsxbStyleSetClipBottom;  
xbStyle.prototype.getClipLeft        = nsxbStyleGetClipLeft;
xbStyle.prototype.setClipLeft        = nsxbStyleSetClipLeft;  
xbStyle.prototype.getClipWidth       = nsxbStyleGetClipWidth;
xbStyle.prototype.setClipWidth       = nsxbStyleSetClipWidth;  
xbStyle.prototype.getClipHeight      = nsxbStyleGetClipHeight;
xbStyle.prototype.setClipHeight      = nsxbStyleSetClipHeight;  
xbStyle.prototype.getLeft            = nsxbStyleGetLeft;
xbStyle.prototype.setLeft            = nsxbStyleSetLeft;
xbStyle.prototype.getTop             = nsxbStyleGetTop;
xbStyle.prototype.setTop             = nsxbStyleSetTop;
xbStyle.prototype.getPageX           = nsxbStyleGetPageX;
xbStyle.prototype.setPageX           = nsxbStyleSetPageX;
xbStyle.prototype.getPageY           = nsxbStyleGetPageY;
xbStyle.prototype.setPageY           = nsxbStyleSetPageY;
xbStyle.prototype.getVisibility      = nsxbStyleGetVisibility;
xbStyle.prototype.setVisibility      = nsxbStyleSetVisibility;
xbStyle.prototype.getzIndex          = nsxbStyleGetzIndex;
xbStyle.prototype.setzIndex          = nsxbStyleSetzIndex;            
xbStyle.prototype.getHeight          = nsxbStyleGetHeight;
xbStyle.prototype.setHeight          = nsxbStyleSetHeight;
xbStyle.prototype.getWidth           = nsxbStyleGetWidth;
xbStyle.prototype.setWidth           = nsxbStyleSetWidth;
xbStyle.prototype.getBackgroundColor = nsxbStyleGetBackgroundColor;
xbStyle.prototype.setBackgroundColor = nsxbStyleSetBackgroundColor;
xbStyle.prototype.getColor           = nsxbStyleGetColor;
xbStyle.prototype.setColor           = nsxbStyleSetColor;
xbStyle.prototype.setInnerHTML       = nsxbSetInnerHTML;
xbStyle.prototype.getBorderTopWidth    = xbStyleNotSupported;
xbStyle.prototype.getBorderRightWidth  = xbStyleNotSupported;
xbStyle.prototype.getBorderBottomWidth = xbStyleNotSupported;
xbStyle.prototype.getBorderLeftWidth   = xbStyleNotSupported;
xbStyle.prototype.getMarginLeft        = xbStyleNotSupported;
xbStyle.prototype.getMarginTop         = xbStyleNotSupported;
xbStyle.prototype.getMarginRight       = xbStyleNotSupported;
xbStyle.prototype.getMarginBottom      = xbStyleNotSupported;
xbStyle.prototype.getMarginLeft        = xbStyleNotSupported;
xbStyle.prototype.getPaddingTop        = xbStyleNotSupported;
xbStyle.prototype.getPaddingRight      = xbStyleNotSupported;
xbStyle.prototype.getPaddingBottom     = xbStyleNotSupported;
xbStyle.prototype.getPaddingLeft       = xbStyleNotSupported;
xbStyle.prototype.getClientWidth       = xbStyleNotSupported;
xbStyle.prototype.getClientHeight      = xbStyleNotSupported;

window.saveInnerWidth = window.innerWidth;
window.saveInnerHeight = window.innerHeight;

window.onresize = nsxbStyleOnresize;

