<?xml version="1.0"?>

<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:exsl="http://exslt.org/common" version="1.0" exclude-result-prefixes="exsl">

<!-- This stylesheet was created by template/titlepage.xsl-->

<xsl:template name="slides.titlepage.recto">
  <xsl:choose>
    <xsl:when test="slidesinfo/title">
      <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/title[1]"/>
    </xsl:when>
    <xsl:when test="docinfo/title">
      <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/title[1]"/>
    </xsl:when>
    <xsl:when test="info/title">
      <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/title[1]"/>
    </xsl:when>
    <xsl:when test="title">
      <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="title[1]"/>
    </xsl:when>
  </xsl:choose>

  <xsl:choose>
    <xsl:when test="slidesinfo/subtitle">
      <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/subtitle[1]"/>
    </xsl:when>
    <xsl:when test="docinfo/subtitle">
      <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/subtitle[1]"/>
    </xsl:when>
    <xsl:when test="info/subtitle">
      <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/subtitle[1]"/>
    </xsl:when>
    <xsl:when test="subtitle">
      <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="subtitle[1]"/>
    </xsl:when>
  </xsl:choose>

  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/corpauthor"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/corpauthor"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/corpauthor"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/authorgroup"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/authorgroup"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/authorgroup"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/author"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/author"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/author"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/pubdate"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/pubdate"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/pubdate"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/confgroup"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/confgroup"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/confgroup"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/releaseinfo"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/releaseinfo"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/releaseinfo"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/copyright"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/copyright"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/copyright"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="slidesinfo/revision"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="docinfo/revision"/>
  <xsl:apply-templates mode="slides.titlepage.recto.auto.mode" select="info/revision"/>
</xsl:template>

<xsl:template name="slides.titlepage.verso">
</xsl:template>

<xsl:template name="slides.titlepage.separator">
</xsl:template>

<xsl:template name="slides.titlepage.before.recto">
</xsl:template>

<xsl:template name="slides.titlepage.before.verso">
</xsl:template>

<xsl:template name="slides.titlepage">
  <fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format">
    <xsl:variable name="recto.content">
      <xsl:call-template name="slides.titlepage.before.recto"/>
      <xsl:call-template name="slides.titlepage.recto"/>
    </xsl:variable>
    <xsl:variable name="recto.elements.count">
      <xsl:choose>
        <xsl:when test="function-available('exsl:node-set')"><xsl:value-of select="count(exsl:node-set($recto.content)/*)"/></xsl:when>
        <xsl:when test="contains(system-property('xsl:vendor'), 'Apache Software Foundation')">
          <!--Xalan quirk--><xsl:value-of select="count(exsl:node-set($recto.content)/*)"/></xsl:when>
        <xsl:otherwise>1</xsl:otherwise>
      </xsl:choose>
    </xsl:variable>
    <xsl:if test="(normalize-space($recto.content) != '') or ($recto.elements.count &gt; 0)">
      <fo:block><xsl:copy-of select="$recto.content"/></fo:block>
    </xsl:if>
    <xsl:variable name="verso.content">
      <xsl:call-template name="slides.titlepage.before.verso"/>
      <xsl:call-template name="slides.titlepage.verso"/>
    </xsl:variable>
    <xsl:variable name="verso.elements.count">
      <xsl:choose>
        <xsl:when test="function-available('exsl:node-set')"><xsl:value-of select="count(exsl:node-set($verso.content)/*)"/></xsl:when>
        <xsl:when test="contains(system-property('xsl:vendor'), 'Apache Software Foundation')">
          <!--Xalan quirk--><xsl:value-of select="count(exsl:node-set($verso.content)/*)"/></xsl:when>
        <xsl:otherwise>1</xsl:otherwise>
      </xsl:choose>
    </xsl:variable>
    <xsl:if test="(normalize-space($verso.content) != '') or ($verso.elements.count &gt; 0)">
      <fo:block><xsl:copy-of select="$verso.content"/></fo:block>
    </xsl:if>
    <xsl:call-template name="slides.titlepage.separator"/>
  </fo:block>
</xsl:template>

<xsl:template match="*" mode="slides.titlepage.recto.mode">
  <!-- if an element isn't found in this mode, -->
  <!-- try the generic titlepage.mode -->
  <xsl:apply-templates select="." mode="titlepage.mode"/>
</xsl:template>

<xsl:template match="*" mode="slides.titlepage.verso.mode">
  <!-- if an element isn't found in this mode, -->
  <!-- try the generic titlepage.mode -->
  <xsl:apply-templates select="." mode="titlepage.mode"/>
</xsl:template>

<xsl:template match="title" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" text-align="center" space-after="1em" padding-top="1.5in" keep-with-next="always" font-size="{$foil.title.size}" font-weight="bold" font-family="{$slide.title.font.family}">
<xsl:call-template name="component.title">
<xsl:with-param name="node" select="ancestor-or-self::slides[1]"/>
</xsl:call-template>
</fo:block>
</xsl:template>

<xsl:template match="subtitle" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" text-align="center" space-after="1em" font-family="{$slide.title.font.family}">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

<xsl:template match="corpauthor" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" font-size="20.736pt" text-align="center" space-after="1em">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

<xsl:template match="authorgroup" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

<xsl:template match="author" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" font-size="20.736pt" text-align="center" space-after="1em">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

<xsl:template match="pubdate" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" font-size="17.28pt" text-align="center" space-after="1em">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

<xsl:template match="confgroup" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" font-size="17.28pt" text-align="center" space-after="1em">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

<xsl:template match="releaseinfo" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" font-size="17.28pt" text-align="center" space-after="1em">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

<xsl:template match="copyright" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" font-size="17.28pt" text-align="center">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

<xsl:template match="revision" mode="slides.titlepage.recto.auto.mode">
<fo:block xmlns:fo="http://www.w3.org/1999/XSL/Format" xsl:use-attribute-sets="slides.titlepage.recto.style" text-align="center">
<xsl:apply-templates select="." mode="slides.titlepage.recto.mode"/>
</fo:block>
</xsl:template>

</xsl:stylesheet>

