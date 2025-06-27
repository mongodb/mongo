/*
 * xbStyle-css.js
 * $Revision: 1.2 $ $Date: 2003/02/07 16:04:21 $
 *
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

// xbStyle.getClip()

function cssStyleGetClip()
{
  var clip = this.getEffectiveValue('clip');

  // hack opera
  if (clip == 'rect()')
    clip = '';

  if (clip == '' || clip == 'auto')
  {
    clip = 'rect(0px, ' + this.getWidth() + 'px, ' + this.getHeight() + 'px, 0px)';
  }
  else
  { 
    clip = clip.replace(/px /g, 'px, ');
  }

  return clip;
}

// xbStyle.setClip()

function cssStyleSetClip(sClipString)
{
  this.styleObj.clip = sClipString;
}

// xbStyle.getClipTop()

function cssStyleGetClipTop()
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  return rect.top;
}

// xbStyle.setClipTop()

function cssStyleSetClipTop(top)
{
  var clip = this.getClip();
  var rect         = new xbClipRect(clip);
  rect.top         = top;
  this.styleObj.clip = rect.toString();
}

// xbStyle.getClipRight()

function cssStyleGetClipRight()
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  return rect.right;
}

// xbStyle.setClipRight()

function cssStyleSetClipRight(right)
{
  var clip = this.getClip();
  var rect          = new xbClipRect(clip);
  rect.right        = right;
  this.styleObj.clip  = rect.toString();
}

// xbStyle.getClipBottom()

function cssStyleGetClipBottom()
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  return rect.bottom;
}

// xbStyle.setClipBottom()

function cssStyleSetClipBottom(bottom)
{
  var clip = this.getClip();
  var rect           = new xbClipRect(clip);
  rect.bottom        = bottom;
  this.styleObj.clip   = rect.toString();
}

// xbStyle.getClipLeft()

function cssStyleGetClipLeft()
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  return rect.left;
}

// xbStyle.setClipLeft()

function cssStyleSetClipLeft(left)
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  rect.left = left;
  this.styleObj.clip = rect.toString();
}

// xbStyle.getClipWidth()

function cssStyleGetClipWidth()
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  return rect.getWidth();
}

// xbStyle.setClipWidth()

function cssStyleSetClipWidth(width)
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  rect.setWidth(width);
  this.styleObj.clip = rect.toString();
}

// xbStyle.getClipHeight()

function cssStyleGetClipHeight()
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  return rect.getHeight();
}

// xbStyle.setClipHeight()

function cssStyleSetClipHeight(height)
{
  var clip = this.getClip();
  var rect = new xbClipRect(clip);
  rect.setHeight(height);
  this.styleObj.clip = rect.toString();
}

// the CSS attributes left,top are for absolutely positioned elements
// measured relative to the containing element.  for relatively positioned
// elements, left,top are measured from the element's normal inline position.
// getLeft(), setLeft() operate on this type of coordinate.
//
// to allow dynamic positioning the getOffsetXXX and setOffsetXXX methods are
// defined to return and set the position of either an absolutely or relatively
// positioned element relative to the containing element.
//
//

// xbStyle.getLeft()

function cssStyleGetLeft()
{
  var left = this.getEffectiveValue('left');
  if (typeof(left) == 'number')
     return left;

  if (left != '' && left.indexOf('px') == -1)
  {
    xbDEBUG.dump('xbStyle.getLeft: Element ID=' + this.object.id + ' does not use pixels as units. left=' + left + ' Click Ok to continue, Cancel to Abort');
    return 0;
  }

  if (top == 'auto' && this.object && typeof(this.object.offsetTop) == 'number')
  {
    left = this.object.offsetTop + 'px';
  }

  if (left == '')
    left = '0px';
      
  return xbToInt(left);
}

// xbStyle.setLeft()

function cssStyleSetLeft(left)
{
  if (typeof(this.styleObj.left) == 'number')
    this.styleObj.left = left;
  else
    this.styleObj.left = left + 'px';
}

// xbStyle.getTop()

function cssStyleGetTop()
{
  var top = this.getEffectiveValue('top');
  if (typeof(top) == 'number')
     return top;

  if (top != '' && top.indexOf('px') == -1)
  {
    xbDEBUG.dump('xbStyle.getTop: Element ID=' + this.object.id + ' does not use pixels as units. top=' + top + ' Click Ok to continue, Cancel to Abort');
    return 0;
  }

  if (top == 'auto' && this.object && typeof(this.object.offsetTop) == 'number')
  {
    top = this.object.offsetTop + 'px';
  }

  if (top == '')
    top = '0px';
      
  return xbToInt(top);
}

// xbStyle.setTop()

function cssStyleSetTop(top)
{
  if (typeof(this.styleObj.top) == 'number')
    this.styleObj.top = top;
  else
    this.styleObj.top = top + 'px';
}

// xbStyle.getPageX()

function cssStyleGetPageX()
{
  var x = 0;
  var elm = this.object;
  var elmstyle;
  var position;
  
  //xxxHack: Due to limitations in Gecko's (0.9.6) ability to determine the 
  // effective position attribute , attempt to use offsetXXX

  if (typeof(elm.offsetLeft) == 'number')
  {
    while (elm)
    {
      x += elm.offsetLeft;
      elm = elm.offsetParent;
    }
  }
  else
  {
    while (elm)
    {
      if (elm.style)
      {
        elmstyle = new xbStyle(elm);
        position = elmstyle.getEffectiveValue('position');
        if (position != '' && position != 'static')
          x += elmstyle.getLeft();
      }
      elm = elm.parentNode;
    }
  }
  
  return x;
}

// xbStyle.setPageX()

function cssStyleSetPageX(x)
{
  var xParent = 0;
  var elm = this.object.parentNode;
  var elmstyle;
  var position;
  
  //xxxHack: Due to limitations in Gecko's (0.9.6) ability to determine the 
  // effective position attribute , attempt to use offsetXXX

  if (elm && typeof(elm.offsetLeft) == 'number')
  {
    while (elm)
    {
      xParent += elm.offsetLeft;
      elm = elm.offsetParent;
    }
  }
  else
  {
    while (elm)
    {
      if (elm.style)
      {
        elmstyle = new xbStyle(elm);
        position = elmstyle.getEffectiveValue('position');
        if (position != '' && position != 'static')
          xParent += elmstyle.getLeft();
      }
      elm = elm.parentNode;
    }
  }
  
  x -= xParent;

  this.setLeft(x);
}
    
// xbStyle.getPageY()

function cssStyleGetPageY()
{
  var y = 0;
  var elm = this.object;
  var elmstyle;
  var position;
  
  //xxxHack: Due to limitations in Gecko's (0.9.6) ability to determine the 
  // effective position attribute , attempt to use offsetXXX

  if (typeof(elm.offsetTop) == 'number')
  {
    while (elm)
    {
      y += elm.offsetTop;
      elm = elm.offsetParent;
    }
  }
  else
  {
    while (elm)
    {
      if (elm.style)
      {
        elmstyle = new xbStyle(elm);
        position = elmstyle.getEffectiveValue('position');
        if (position != '' && position != 'static')
          y += elmstyle.getTop();
      }
      elm = elm.parentNode;
    }
  }
  
  return y;
}

// xbStyle.setPageY()

function cssStyleSetPageY(y)
{
  var yParent = 0;
  var elm = this.object.parentNode;
  var elmstyle;
  var position;
  
  //xxxHack: Due to limitations in Gecko's (0.9.6) ability to determine the 
  // effective position attribute , attempt to use offsetXXX

  if (elm && typeof(elm.offsetTop) == 'number')
  {
    while (elm)
    {
      yParent += elm.offsetTop;
      elm = elm.offsetParent;
    }
  }
  else
  {
    while (elm)
    {
      if (elm.style)
      {
        elmstyle = new xbStyle(elm);
        position = elmstyle.getEffectiveValue('position');
        if (position != '' && position != 'static')
          yParent += elmstyle.getTop();
      }
      elm = elm.parentNode;
    }
  }
  
  y -= yParent;

  this.setTop(y);
}
    
// xbStyle.getHeight()

function cssStyleGetHeight()
{
  var display = this.getEffectiveValue('display');
  var height = this.getEffectiveValue('height');

  if (typeof(height) == 'number')
  {
     // Opera
     return height;
  }

  if (height == '' || height == 'auto' || height.indexOf('%') != -1)
  {
    if (typeof(this.object.offsetHeight) == 'number')
    {
      height = this.object.offsetHeight + 'px';
    }
    else if (typeof(this.object.scrollHeight) == 'number')
    {
      height = this.object.scrollHeight + 'px';
    }
  }

  if (height.indexOf('px') == -1)
  {
    xbDEBUG.dump('xbStyle.getHeight: Element ID=' + this.object.id + ' does not use pixels as units. height=' + height + ' Click Ok to continue, Cancel to Abort');
    return 0;
  }

  height = xbToInt(height);

  return height;
}

// xbStyle.setHeight()

function cssStyleSetHeight(height)
{
  if (typeof(this.styleObj.height) == 'number')
    this.styleObj.height = height;
  else
    this.styleObj.height = height + 'px';
}

// xbStyle.getWidth()

function cssStyleGetWidth()
{
  var display = this.getEffectiveValue('display');
  var width = this.getEffectiveValue('width');

  if (typeof(width) == 'number')
  {
     // note Opera 6 has a bug in width and offsetWidth where 
     // it returns the page width. Use clientWidth instead.
     if (navigator.userAgent.indexOf('Opera') != -1)
       return this.object.clientWidth;
     else
       return width;
  }

  if (width == '' || width == 'auto' || width.indexOf('%') != -1)
  {
    if (typeof(this.object.offsetWidth) == 'number')
    {
      width = this.object.offsetWidth + 'px';
    }
    else if (typeof(this.object.scrollHeight) == 'number')
    {
      width = this.object.scrollWidth + 'px';
    }
  }

  if (width.indexOf('px') == -1)
  {
    xbDEBUG.dump('xbStyle.getWidth: Element ID=' + this.object.id + ' does not use pixels as units. width=' + width + ' Click Ok to continue, Cancel to Abort');
    return 0;
  }

  width = xbToInt(width);

  return width;
}

// xbStyle.setWidth()

function cssStyleSetWidth(width)
{
  if (typeof(this.styleObj.width) == 'number')
    this.styleObj.width = width;
  else
    this.styleObj.width = width + 'px';
}

// xbStyle.getVisibility()

function cssStyleGetVisibility()
{
  return this.getEffectiveValue('visibility');
}

// xbStyle.setVisibility()

function cssStyleSetVisibility(visibility)
{
  this.styleObj.visibility = visibility;
}

// xbStyle.getzIndex()

function cssStyleGetzIndex()
{
  return xbToInt(this.getEffectiveValue('zIndex'));
}

// xbStyle.setzIndex()

function cssStyleSetzIndex(zIndex)
{
  this.styleObj.zIndex = zIndex;
}

// xbStyle.getBackgroundColor()

function cssStyleGetBackgroundColor()
{
  return this.getEffectiveValue('backgroundColor');
}

// xbStyle.setBackgroundColor()

function cssStyleSetBackgroundColor(color)
{
  this.styleObj.backgroundColor = color;
}

// xbStyle.getColor()

function cssStyleGetColor()
{
  return this.getEffectiveValue('color');
}

// xbStyle.setColor()

function cssStyleSetColor(color)
{
  this.styleObj.color = color;
}

// xbStyle.moveAbove()

function xbStyleMoveAbove(cont)
{
  this.setzIndex(cont.getzIndex()+1);
}

// xbStyle.moveBelow()

function xbStyleMoveBelow(cont)
{
  var zindex = cont.getzIndex() - 1;
            
  this.setzIndex(zindex);
}

// xbStyle.moveBy()

function xbStyleMoveBy(deltaX, deltaY)
{
  this.moveTo(this.getLeft() + deltaX, this.getTop() + deltaY);
}

// xbStyle.moveTo()

function xbStyleMoveTo(x, y)
{
  this.setLeft(x);
  this.setTop(y);
}

// xbStyle.moveToAbsolute()

function xbStyleMoveToAbsolute(x, y)
{
  this.setPageX(x);
  this.setPageY(y);
}

// xbStyle.resizeBy()

function xbStyleResizeBy(deltaX, deltaY)
{
  this.setWidth( this.getWidth() + deltaX );
  this.setHeight( this.getHeight() + deltaY );
}

// xbStyle.resizeTo()

function xbStyleResizeTo(x, y)
{
  this.setWidth(x);
  this.setHeight(y);
}

// xbStyle.setInnerHTML()

function xbSetInnerHTML(str)
{
  if (typeof(this.object.innerHTML) != 'undefined')
    this.object.innerHTML = str;
}


// Extensions to xbStyle that are not supported by Netscape Navigator 4
// but that provide cross browser implementations of properties for 
// Mozilla, Gecko, Netscape 6.x and Opera

// xbStyle.getBorderTopWidth()

function cssStyleGetBorderTopWidth()
{
  return xbToInt(this.getEffectiveValue('borderTopWidth'));
}

// xbStyle.getBorderRightWidth()

function cssStyleGetBorderRightWidth()
{
  return xbToInt(this.getEffectiveValue('borderRightWidth'));
}

// xbStyle.getBorderBottomWidth()

function cssStyleGetBorderBottomWidth()
{
  return xbToInt(this.getEffectiveValue('borderBottomWidth'));
}

// xbStyle.getBorderLeftWidth()

function cssStyleGetBorderLeftWidth()
{
  return xbToInt(this.getEffectiveValue('borderLeftWidth'));
}

// xbStyle.getMarginTop()

function cssStyleGetMarginTop()
{
  return xbToInt(this.getEffectiveValue('marginTop'));
}

// xbStyle.getMarginRight()

function cssStyleGetMarginRight()
{
  return xbToInt(this.getEffectiveValue('marginRight'));
}

// xbStyle.getMarginBottom()

function cssStyleGetMarginBottom()
{
  return xbToInt(this.getEffectiveValue('marginBottom'));
}

// xbStyle.getMarginLeft()

function cssStyleGetMarginLeft()
{
  return xbToInt(this.getEffectiveValue('marginLeft'));
}

// xbStyle.getPaddingTop()

function cssStyleGetPaddingTop()
{
  return xbToInt(this.getEffectiveValue('paddingTop'));
}

// xbStyle.getPaddingRight()

function cssStyleGetPaddingRight()
{
  return xbToInt(this.getEffectiveValue('paddingRight'));
}

// xbStyle.getPaddingBottom()

function cssStyleGetPaddingBottom()
{
  return xbToInt(this.getEffectiveValue('paddingBottom'));
}

// xbStyle.getPaddingLeft()

function cssStyleGetPaddingLeft()
{
  return xbToInt(this.getEffectiveValue('paddingLeft'));
}

// xbStyle.getClientWidth()

function cssStyleGetClientWidth()
{
  return this.getWidth() + this.getPaddingLeft() + this.getPaddingRight();
  /*
  if (typeof(this.object.clientWidth) == 'number')
    return this.object.clientWidth;

  return null;
    */
}

// xbStyle.getClientHeight()

function cssStyleGetClientHeight()
{
  return this.getHeight() + this.getPaddingTop() + this.getPaddingBottom();
  /*
  if (typeof(this.object.clientHeight) == 'number')
    return this.object.clientHeight;

  return null;
  */
}

xbStyle.prototype.getClip            = cssStyleGetClip;
xbStyle.prototype.setClip            = cssStyleSetClip;  
xbStyle.prototype.getClipTop         = cssStyleGetClipTop;
xbStyle.prototype.setClipTop         = cssStyleSetClipTop;  
xbStyle.prototype.getClipRight       = cssStyleGetClipRight;
xbStyle.prototype.setClipRight       = cssStyleSetClipRight;  
xbStyle.prototype.getClipBottom      = cssStyleGetClipBottom;
xbStyle.prototype.setClipBottom      = cssStyleSetClipBottom;  
xbStyle.prototype.getClipLeft        = cssStyleGetClipLeft;
xbStyle.prototype.setClipLeft        = cssStyleSetClipLeft;  
xbStyle.prototype.getClipWidth       = cssStyleGetClipWidth;
xbStyle.prototype.setClipWidth       = cssStyleSetClipWidth;  
xbStyle.prototype.getClipHeight      = cssStyleGetClipHeight;
xbStyle.prototype.setClipHeight      = cssStyleSetClipHeight;  
xbStyle.prototype.getLeft            = cssStyleGetLeft;
xbStyle.prototype.setLeft            = cssStyleSetLeft;
xbStyle.prototype.getTop             = cssStyleGetTop;
xbStyle.prototype.setTop             = cssStyleSetTop;
xbStyle.prototype.getPageX           = cssStyleGetPageX;
xbStyle.prototype.setPageX           = cssStyleSetPageX;
xbStyle.prototype.getPageY           = cssStyleGetPageY;
xbStyle.prototype.setPageY           = cssStyleSetPageY;
xbStyle.prototype.getVisibility      = cssStyleGetVisibility;
xbStyle.prototype.setVisibility      = cssStyleSetVisibility;
xbStyle.prototype.getzIndex          = cssStyleGetzIndex;
xbStyle.prototype.setzIndex          = cssStyleSetzIndex;            
xbStyle.prototype.getHeight          = cssStyleGetHeight;
xbStyle.prototype.setHeight          = cssStyleSetHeight;
xbStyle.prototype.getWidth           = cssStyleGetWidth;
xbStyle.prototype.setWidth           = cssStyleSetWidth;
xbStyle.prototype.getBackgroundColor = cssStyleGetBackgroundColor;
xbStyle.prototype.setBackgroundColor = cssStyleSetBackgroundColor;
xbStyle.prototype.getColor           = cssStyleGetColor;
xbStyle.prototype.setColor           = cssStyleSetColor;
xbStyle.prototype.setInnerHTML       = xbSetInnerHTML;
xbStyle.prototype.getBorderTopWidth    = cssStyleGetBorderTopWidth;
xbStyle.prototype.getBorderRightWidth  = cssStyleGetBorderRightWidth;
xbStyle.prototype.getBorderBottomWidth = cssStyleGetBorderBottomWidth;
xbStyle.prototype.getBorderLeftWidth   = cssStyleGetBorderLeftWidth;
xbStyle.prototype.getMarginLeft        = cssStyleGetMarginLeft;
xbStyle.prototype.getMarginTop         = cssStyleGetMarginTop;
xbStyle.prototype.getMarginRight       = cssStyleGetMarginRight;
xbStyle.prototype.getMarginBottom      = cssStyleGetMarginBottom;
xbStyle.prototype.getMarginLeft        = cssStyleGetMarginLeft;
xbStyle.prototype.getPaddingTop        = cssStyleGetPaddingTop;
xbStyle.prototype.getPaddingRight      = cssStyleGetPaddingRight;
xbStyle.prototype.getPaddingBottom     = cssStyleGetPaddingBottom;
xbStyle.prototype.getPaddingLeft       = cssStyleGetPaddingLeft;
xbStyle.prototype.getClientWidth       = cssStyleGetClientWidth;
xbStyle.prototype.getClientHeight      = cssStyleGetClientHeight;

