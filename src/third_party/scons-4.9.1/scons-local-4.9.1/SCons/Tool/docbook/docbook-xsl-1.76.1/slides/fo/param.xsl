<?xml version="1.0" encoding="ASCII"?>
<!-- This file is generated from param.xweb -->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">

<!-- ********************************************************************
     $Id: param.xweb 6633 2007-02-21 18:33:33Z xmldoc $
     ********************************************************************

     This file is part of the DocBook Slides Stylesheet distribution.
     See ../README or http://docbook.sf.net/release/xsl/current/ for
     copyright and other information.

     ******************************************************************** -->

<xsl:param name="slide.font.family">Helvetica</xsl:param>
<xsl:param name="slide.title.font.family">Helvetica</xsl:param>
<xsl:param name="foil.title.master">36</xsl:param>
<!-- Inconsistant use of point size? -->
    <xsl:param name="foil.title.size">
      <xsl:value-of select="$foil.title.master"/><xsl:text>pt</xsl:text>
    </xsl:param>
  
    <xsl:attribute-set name="foilgroup.properties">
      <xsl:attribute name="font-family">
        <xsl:value-of select="$slide.font.family"/>
      </xsl:attribute>
    </xsl:attribute-set>
  
    <xsl:attribute-set name="foil.properties">
      <xsl:attribute name="font-family">
        <xsl:value-of select="$slide.font.family"/>
      </xsl:attribute>
      <xsl:attribute name="margin-{$direction.align.start}">1in</xsl:attribute>
      <xsl:attribute name="margin-{$direction.align.end}">1in</xsl:attribute>
      <xsl:attribute name="font-size">
        <xsl:value-of select="$body.font.size"/>
      </xsl:attribute>
      <xsl:attribute name="font-weight">bold</xsl:attribute>
    </xsl:attribute-set>
  
    <xsl:attribute-set name="foil.subtitle.properties">
      <xsl:attribute name="font-family">
        <xsl:value-of select="$slide.title.font.family"/>
      </xsl:attribute>
      <xsl:attribute name="text-align">center</xsl:attribute>
      <xsl:attribute name="font-size">
        <xsl:value-of select="$foil.title.master * 0.8"/><xsl:text>pt</xsl:text>
      </xsl:attribute>
      <xsl:attribute name="space-after">12pt</xsl:attribute>
    </xsl:attribute-set>
  
    <xsl:attribute-set name="running.foot.properties">
      <xsl:attribute name="font-family">
        <xsl:value-of select="$slide.font.family"/>
      </xsl:attribute>
      <xsl:attribute name="font-size">14pt</xsl:attribute>
      <xsl:attribute name="color">#9F9F9F</xsl:attribute>
    </xsl:attribute-set>
  
    <xsl:attribute-set name="speakernote.properties">
      <xsl:attribute name="font-family">Times Roman</xsl:attribute>
      <xsl:attribute name="font-style">italic</xsl:attribute>
      <xsl:attribute name="font-size">12pt</xsl:attribute>
      <xsl:attribute name="font-weight">normal</xsl:attribute>
    </xsl:attribute-set>
  
    <xsl:attribute-set name="slides.properties">
      <xsl:attribute name="font-family">
        <xsl:value-of select="$slide.font.family"/>
      </xsl:attribute>
    </xsl:attribute-set>
  

</xsl:stylesheet>
