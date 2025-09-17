/*
 * CTOCWidget.js
 * $Revision: 1.3 $ $Date: 2003/07/14 06:02:50 $
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
 * Portions created by the Initial Developer are Copyright (C) 2003
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s): Bob Clary <bclary@netscape.com>
 *
 * ***** END LICENSE BLOCK ***** */

function CTOCWidget(domTOCModel, target)
{
  if (domTOCModel.documentElement.nodeName != 'toc')
  {
    throw 'CTOCWidget called on non toc Document: ' + domTOCModel.nodeName;
  }

  this.model = domTOCModel;
  this.target = target;
  this.view = document.createElement('div');
  this.view.setAttribute('class', CTOCWidget._classprefix + '_view');

  var modelItems = domTOCModel.documentElement.childNodes;
  for (var i = 0; i < modelItems.length; i++)
  {
    var modelItem = modelItems.item(i);
    if (modelItem.nodeType == Node.ELEMENT_NODE)
    {
      var viewItem  = CTOCWidget.createItemView(modelItem, target);
      this.view.appendChild(viewItem);
    }
  }
}

CTOCWidget._handleImages  = { open: '/toolbox/examples/2003/CTOCWidget/minus.gif', closed: '/toolbox/examples/2003/CTOCWidget/plus.gif', height: '12px', width: '16px'};
CTOCWidget._classprefix  = 'CTOCWidget';

CTOCWidget.createItemView = function (modelItem, target)
{
  if (modelItem.nodeType != Node.ELEMENT_NODE)
  {
    throw 'CTOCWidget.createItemView called on non-Element: ' + modelItem.nodeName;
  }

  var i;

  var viewItem = document.createElement('div');
  viewItem.setAttribute('class', CTOCWidget._classprefix + '_item');

  var viewItemHandle = document.createElement('div');
  viewItemHandle.setAttribute('class', CTOCWidget._classprefix + '_itemhandle');
  viewItemHandle.style.cursor = 'pointer';

  var viewItemHandleImg = document.createElement('img');
  viewItemHandleImg.style.height = CTOCWidget._handleImages.height;
  viewItemHandleImg.style.width = CTOCWidget._handleImages.width;
  viewItemHandleImg.addEventListener('click', CTOCWidget.toggleHandle, false);

  var viewItemHandleLink;
  if (!modelItem.getAttribute('url'))
  {
    viewItemHandleLink = document.createElement('span');
  }
  else 
  {
    viewItemHandleLink = document.createElement('a');
    viewItemHandleLink.setAttribute('href', modelItem.getAttribute('url'));
    viewItemHandleLink.setAttribute('target', target);
  }
  viewItemHandleLink.appendChild(document.createTextNode(modelItem.getAttribute('title')));

  viewItemHandle.appendChild(viewItemHandleImg);
  viewItemHandle.appendChild(viewItemHandleLink);
  viewItem.appendChild(viewItemHandle);

  if (modelItem.childNodes.length == 0)
  {
    viewItemHandleImg.setAttribute('src', CTOCWidget._handleImages.open);
  }
  else
  {
    viewItemHandleImg.setAttribute('src', CTOCWidget._handleImages.closed);

    var viewItemChildren = document.createElement('div');
    viewItemChildren.setAttribute('class', CTOCWidget._classprefix + '_itemchildren');
    viewItemChildren.style.display = 'none';
    viewItemChildren.style.position = 'relative';
    viewItemChildren.style.left = '1em';

    for (i = 0; i < modelItem.childNodes.length; i++)
    {
      var modelItemChild = modelItem.childNodes.item(i);
      if (modelItemChild.nodeType == Node.ELEMENT_NODE)
      {
        viewItemChildren.appendChild(CTOCWidget.createItemView(modelItemChild, target));
      }
    }

    viewItem.appendChild(viewItemChildren);
  }

  return viewItem;
};

// fires on img part of the handle
CTOCWidget.toggleHandle = function(e)
{
  switch (e.eventPhase)
  {
    case Event.CAPTURING_PHASE:
    case Event.BUBBLING_PHASE:
      return true;
    
    case Event.AT_TARGET:
     
      e.preventBubble();

      var domHandle   = e.target.parentNode;
      var domChildren = domHandle.nextSibling;

      if (!domChildren)
      {
        return true;
      }

      switch(domChildren.style.display)
      {
        case '':
        case 'block':
          domChildren.style.display = 'none';
          e.target.setAttribute('src', CTOCWidget._handleImages.closed);
          break;
        case 'none':
          domChildren.style.display = 'block';
          e.target.setAttribute('src', CTOCWidget._handleImages.open);
          break;
        default:
          return false;
       }

       return true;

    default:
      dump('Unknown Event Phase ' + e.eventPhase);
      break;
  }

  return true;
}

