<?xml version="1.0"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:fo="http://www.w3.org/1999/XSL/Format"
                xmlns:rx="http://www.renderx.com/XSL/Extensions"
                version="1.0">

<!-- ********************************************************************
     $Id: plain.xsl 8101 2008-08-03 18:35:14Z mzjn $
     ********************************************************************

     This file is part of the DocBook Slides Stylesheet distribution.
     See ../README or http://docbook.sf.net/release/xsl/current/ for
     copyright and other information.

     ******************************************************************** -->

<xsl:import href="../../fo/docbook.xsl"/>
<xsl:import href="param.xsl"/>

<xsl:param name="alignment" select="'start'"/>

<xsl:include href="plain-titlepage.xsl"/>

<xsl:param name="local.l10n.xml" select="document('')"/>
<i18n xmlns="http://docbook.sourceforge.net/xmlns/l10n/1.0">
  <l:l10n xmlns:l="http://docbook.sourceforge.net/xmlns/l10n/1.0" language="en">
    <l:gentext key="Continued" text="(Continued)"/>
    <l:context name="title">
      <l:template name="slides" text="%t"/>
      <l:template name="foilgroup" text="%t"/>
      <l:template name="foil" text="%t"/>
    </l:context>
  </l:l10n>
</i18n>

<xsl:variable name="root.elements" select="' slides '"/>

<xsl:param name="preferred.mediaobject.role" select="'print'"/>

<xsl:param name="page.orientation" select="'landscape'"/>

<xsl:param name="body.font.master" select="24"/>

<xsl:attribute-set name="formal.title.properties"
                   use-attribute-sets="normal.para.spacing">
  <xsl:attribute name="font-weight">bold</xsl:attribute>
  <xsl:attribute name="font-size">
    <xsl:value-of select="$body.font.master * 1.2"/>
    <xsl:text>pt</xsl:text>
  </xsl:attribute>
  <xsl:attribute name="hyphenate">false</xsl:attribute>
  <xsl:attribute name="space-after.minimum">8pt</xsl:attribute>
  <xsl:attribute name="space-after.optimum">6pt</xsl:attribute>
  <xsl:attribute name="space-after.maximum">10pt</xsl:attribute>
</xsl:attribute-set>

<xsl:attribute-set name="list.block.spacing">
  <xsl:attribute name="space-before.optimum">12pt</xsl:attribute>
  <xsl:attribute name="space-before.minimum">8pt</xsl:attribute>
  <xsl:attribute name="space-before.maximum">14pt</xsl:attribute>
  <xsl:attribute name="space-after.optimum">0pt</xsl:attribute>
  <xsl:attribute name="space-after.minimum">0pt</xsl:attribute>
  <xsl:attribute name="space-after.maximum">0pt</xsl:attribute>
</xsl:attribute-set>

<xsl:attribute-set name="list.item.spacing">
  <xsl:attribute name="space-before.optimum">6pt</xsl:attribute>
  <xsl:attribute name="space-before.minimum">4pt</xsl:attribute>
  <xsl:attribute name="space-before.maximum">8pt</xsl:attribute>
</xsl:attribute-set>

<xsl:attribute-set name="normal.para.spacing">
  <xsl:attribute name="space-before.optimum">8pt</xsl:attribute>
  <xsl:attribute name="space-before.minimum">6pt</xsl:attribute>
  <xsl:attribute name="space-before.maximum">10pt</xsl:attribute>
</xsl:attribute-set>

<xsl:attribute-set name="slides.titlepage.recto.style">
  <xsl:attribute name="font-family">
    <xsl:value-of select="$slide.font.family"/>
  </xsl:attribute>
</xsl:attribute-set>

<xsl:attribute-set name="slides.titlepage.verso.style">
  <xsl:attribute name="font-family">
    <xsl:value-of select="$slide.font.family"/>
  </xsl:attribute>
</xsl:attribute-set>

<!-- ============================================================ -->

<xsl:param name="page.margin.top" select="'0.25in'"/>
<xsl:param name="region.before.extent" select="'0.75in'"/>
<xsl:param name="body.margin.top" select="'1in'"/>

<xsl:param name="region.after.extent" select="'0.5in'"/>
<xsl:param name="body.margin.bottom" select="'0.5in'"/>
<xsl:param name="page.margin.bottom" select="'0.25in'"/>

<xsl:param name="page.margin.inner" select="'0.25in'"/>
<xsl:param name="page.margin.outer" select="'0.25in'"/>
<xsl:param name="column.count.body" select="1"/>

<xsl:template name="user.pagemasters">
  <fo:simple-page-master master-name="slides-titlepage-master"
                         page-width="{$page.width}"
                         page-height="{$page.height}"
                         margin-top="{$page.margin.top}"
                         margin-bottom="{$page.margin.bottom}"
                         margin-left="{$page.margin.inner}"
                         margin-right="{$page.margin.outer}">
    <fo:region-body margin-bottom="0pt"
                    margin-top="0pt"
                    column-count="{$column.count.body}">
    </fo:region-body>
  </fo:simple-page-master>

  <fo:simple-page-master master-name="slides-foil-master"
                         page-width="{$page.width}"
                         page-height="{$page.height}"
                         margin-top="{$page.margin.top}"
                         margin-bottom="{$page.margin.bottom}"
                         margin-left="{$page.margin.inner}"
                         margin-right="{$page.margin.outer}">
    <fo:region-body margin-bottom="{$body.margin.bottom}"
                    margin-top="{$body.margin.top}"
                    column-count="{$column.count.body}">
    </fo:region-body>
    <fo:region-before region-name="xsl-region-before-foil"
                      extent="{$region.before.extent}"
                      display-align="before"/>
    <fo:region-after region-name="xsl-region-after-foil"
                     extent="{$region.after.extent}"
                     display-align="after"/>
  </fo:simple-page-master>

  <fo:simple-page-master master-name="slides-foil-continued-master"
                         page-width="{$page.width}"
                         page-height="{$page.height}"
                         margin-top="{$page.margin.top}"
                         margin-bottom="{$page.margin.bottom}"
                         margin-left="{$page.margin.inner}"
                         margin-right="{$page.margin.outer}">
    <fo:region-body margin-bottom="{$body.margin.bottom}"
                    margin-top="{$body.margin.top}"
                    column-count="{$column.count.body}">
    </fo:region-body>
    <fo:region-before region-name="xsl-region-before-foil-continued"
                      extent="{$region.before.extent}"
                      display-align="before"/>
    <fo:region-after region-name="xsl-region-after-foil-continued"
                     extent="{$region.after.extent}"
                     display-align="after"/>
  </fo:simple-page-master>

  <fo:page-sequence-master master-name="slides-titlepage">
    <fo:repeatable-page-master-alternatives>
      <fo:conditional-page-master-reference master-reference="slides-titlepage-master"/>
    </fo:repeatable-page-master-alternatives>
  </fo:page-sequence-master>

  <fo:page-sequence-master master-name="slides-foil">
    <fo:repeatable-page-master-alternatives>
      <fo:conditional-page-master-reference master-reference="slides-foil-master"
                                            page-position="first"/>
      <fo:conditional-page-master-reference master-reference="slides-foil-continued-master"/>
    </fo:repeatable-page-master-alternatives>
  </fo:page-sequence-master>
</xsl:template>

<xsl:template match="*" mode="running.head.mode">
  <xsl:param name="master-reference" select="'unknown'"/>
  <!-- use the foilgroup title if there is one -->
  <fo:static-content flow-name="xsl-region-before-foil">
    <fo:block background-color="white"
              color="black"
              font-size="{$foil.title.size}"
              font-weight="bold"
              text-align="center"
              font-family="{$slide.title.font.family}">
      <xsl:apply-templates select="title" mode="titlepage.mode"/>
    </fo:block>
  </fo:static-content>

  <fo:static-content flow-name="xsl-region-before-foil-continued">
    <fo:block background-color="white"
              color="black"
              font-size="{$foil.title.size}"
              font-weight="bold"
              text-align="center"
              font-family="{$slide.title.font.family}">
      <xsl:apply-templates select="title" mode="titlepage.mode"/>
      <xsl:text> </xsl:text>
      <xsl:call-template name="gentext">
        <xsl:with-param name="key" select="'Continued'"/>
      </xsl:call-template>
    </fo:block>
  </fo:static-content>
</xsl:template>

<xsl:template match="*" mode="running.foot.mode">
  <xsl:param name="master-reference" select="'unknown'"/>

  <xsl:variable name="last-slide"
                select="(//foil|//foilgroup)[last()]"/>

  <xsl:variable name="last-id">
    <xsl:choose>
      <xsl:when test="$last-slide/@id">
        <xsl:value-of select="$last-slide/@id"/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="generate-id($last-slide)"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:variable>

  <xsl:variable name="content">
    <fo:table table-layout="fixed" width="100%"
              xsl:use-attribute-sets="running.foot.properties">
      <fo:table-column column-number="1" column-width="33%"/>
      <fo:table-column column-number="2" column-width="34%"/>
      <fo:table-column column-number="3" column-width="33%"/>
      <fo:table-body>
        <fo:table-row height="14pt">
          <fo:table-cell text-align="left">
            <fo:block>
              <xsl:if test="self::foil">
                <xsl:choose>
                  <xsl:when test="ancestor::foilgroup[1]/titleabbrev">
                    <xsl:apply-templates select="ancestor::foilgroup[1]/titleabbrev"
                                         mode="titlepage.mode"/>
                  </xsl:when>
                  <xsl:otherwise>
                    <xsl:apply-templates select="ancestor::foilgroup[1]/title"
                                         mode="titlepage.mode"/>
                  </xsl:otherwise>
                </xsl:choose>
              </xsl:if>
            </fo:block>
          </fo:table-cell>
          <fo:table-cell text-align="center">
            <fo:block>
              <xsl:if test="/slides/slidesinfo/releaseinfo[@role='copyright']">
                <xsl:apply-templates select="/slides/slidesinfo/releaseinfo[@role='copyright']"
                                     mode="value"/>
                <xsl:text>&#160;&#160;&#160;</xsl:text>
              </xsl:if>
              <xsl:apply-templates select="/slides/slidesinfo/copyright"
                                   mode="titlepage.mode"/>
            </fo:block>
          </fo:table-cell>
          <fo:table-cell text-align="right">
            <fo:block>
              <fo:page-number/>
              <xsl:text>&#160;/&#160;</xsl:text>
              <fo:page-number-citation ref-id="{$last-id}"/>
            </fo:block>
          </fo:table-cell>
        </fo:table-row>
      </fo:table-body>
    </fo:table>
  </xsl:variable>

  <fo:static-content flow-name="xsl-region-after-foil">
    <fo:block>
      <xsl:copy-of select="$content"/>
    </fo:block>
  </fo:static-content>

  <fo:static-content flow-name="xsl-region-after-foil-continued">
    <fo:block>
      <xsl:copy-of select="$content"/>
    </fo:block>
  </fo:static-content>
</xsl:template>

<xsl:template name="select.user.pagemaster">
  <xsl:param name="element"/>
  <xsl:param name="pageclass"/>
  <xsl:param name="default-pagemaster"/>

  <xsl:choose>
    <xsl:when test="$element = 'slides'">slides-titlepage</xsl:when>
    <xsl:otherwise>slides-foil</xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template match="slides">
  <xsl:variable name="master-reference">
    <xsl:call-template name="select.pagemaster"/>
  </xsl:variable>

  <fo:page-sequence hyphenate="{$hyphenate}"
                    master-reference="{$master-reference}">
    <xsl:attribute name="language">
      <xsl:call-template name="l10n.language"/>
    </xsl:attribute>

    <xsl:apply-templates select="." mode="running.head.mode">
      <xsl:with-param name="master-reference" select="$master-reference"/>
    </xsl:apply-templates>
    <xsl:apply-templates select="." mode="running.foot.mode">
      <xsl:with-param name="master-reference" select="$master-reference"/>
    </xsl:apply-templates>
    <fo:flow flow-name="xsl-region-body">
      <fo:block>
        <xsl:call-template name="anchor">
          <xsl:with-param name="conditional" select="0"/>
        </xsl:call-template>
        <xsl:call-template name="slides.titlepage"/>
        <xsl:apply-templates select="speakernotes"/>
      </fo:block>
    </fo:flow>
  </fo:page-sequence>
  <xsl:apply-templates select="foil|foilgroup"/>
</xsl:template>

<xsl:template match="slidesinfo"/>

<xsl:template match="slides" mode="title.markup">
  <xsl:param name="allow-anchors" select="'0'"/>
  <xsl:apply-templates select="(slidesinfo/title|title)[1]"
                       mode="title.markup">
    <xsl:with-param name="allow-anchors" select="$allow-anchors"/>
  </xsl:apply-templates>
</xsl:template>

<!-- ============================================================ -->

<xsl:template name="foilgroup.titlepage">
  <fo:block background-color="black"
            color="white"
            font-size="{$foil.title.size}"
            font-weight="bold"
            text-align="center"
            padding-top="12pt"
            padding-bottom="12pt"
            space-after="1em">
    <xsl:apply-templates select="title" mode="titlepage.mode"/>
  </fo:block>
</xsl:template>

<xsl:template match="foilgroup">
  <xsl:variable name="master-reference">
    <xsl:call-template name="select.pagemaster"/>
  </xsl:variable>

  <fo:page-sequence hyphenate="{$hyphenate}"
                    master-reference="{$master-reference}">
    <xsl:call-template name="anchor">
      <xsl:with-param name="conditional" select="0"/>
    </xsl:call-template>
    <xsl:attribute name="language">
      <xsl:call-template name="l10n.language"/>
    </xsl:attribute>

    <xsl:apply-templates select="." mode="running.head.mode">
      <xsl:with-param name="master-reference" select="$master-reference"/>
    </xsl:apply-templates>
    <xsl:apply-templates select="." mode="running.foot.mode">
      <xsl:with-param name="master-reference" select="$master-reference"/>
    </xsl:apply-templates>

    <fo:flow flow-name="xsl-region-body">
      <fo:block>
	<xsl:if test="*[not(self::foil)]">
	  <fo:block xsl:use-attribute-sets="foil.properties" space-after="1em">
	    <xsl:apply-templates select="*[not(self::foil)]"/>
	  </fo:block>
	</xsl:if>

	<xsl:call-template name="foilgroup.titlepage"/>
      </fo:block>
    </fo:flow>
  </fo:page-sequence>
  <xsl:apply-templates select="foil"/>
</xsl:template>

<xsl:template match="foilgroup/title"/>
<xsl:template match="foilgroup/titleabbrev"/>

<xsl:template match="foilgroup/titleabbrev" mode="titlepage.mode">
  <xsl:apply-templates/>
</xsl:template>

<xsl:template match="slides/foilgroup/title" mode="titlepage.mode">
  <xsl:apply-templates/>
</xsl:template>

<xsl:template match="title" mode="foilgroup.titlepage.recto.mode">
  <fo:block>
    <fo:inline color="white">.</fo:inline>
    <fo:block space-before="2in">
      <xsl:apply-templates select="." mode="titlepage.mode"/>
    </fo:block>
  </fo:block>
</xsl:template>

<xsl:template match="foilgroupinfo"/>

<!-- ============================================================ -->

<!--
<xsl:template name="foil.titlepage">
  <fo:block background-color="white"
            color="black"
            font-size="{$foil.title.size}"
            font-weight="bold"
            text-align="center"
            padding-top="12pt"
            padding-bottom="12pt"
            space-after="1em">
    <xsl:apply-templates select="title" mode="titlepage.mode"/>
  </fo:block>
</xsl:template>
-->

<xsl:template match="foil">
  <xsl:variable name="master-reference">
    <xsl:call-template name="select.pagemaster"/>
  </xsl:variable>

  <fo:page-sequence hyphenate="{$hyphenate}"
                    master-reference="{$master-reference}">
    <xsl:call-template name="anchor">
      <xsl:with-param name="conditional" select="0"/>
    </xsl:call-template>
    <xsl:attribute name="language">
      <xsl:call-template name="l10n.language"/>
    </xsl:attribute>

    <xsl:apply-templates select="." mode="running.head.mode">
      <xsl:with-param name="master-reference" select="$master-reference"/>
    </xsl:apply-templates>
    <xsl:apply-templates select="." mode="running.foot.mode">
      <xsl:with-param name="master-reference" select="$master-reference"/>
    </xsl:apply-templates>
    <fo:flow flow-name="xsl-region-body">
      <fo:block>
	<fo:block xsl:use-attribute-sets="foil.properties">
          <xsl:apply-templates/>
        </fo:block>
      </fo:block>
    </fo:flow>
  </fo:page-sequence>
</xsl:template>

<xsl:template match="foilinfo"/>
<xsl:template match="foil/title"/>
<xsl:template match="foil/subtitle">
  <fo:block xsl:use-attribute-sets="foil.subtitle.properties">
    <xsl:apply-templates/>
  </fo:block>
</xsl:template>
<xsl:template match="foil/titleabbrev"/>

<!-- ============================================================ -->

<xsl:template match="slides" mode="label.markup">
  <xsl:if test="@label">
    <xsl:value-of select="@label"/>
  </xsl:if>
</xsl:template>

<!-- ============================================================ -->

<xsl:template match="speakernotes">
  <fo:block xsl:use-attribute-sets="speakernote.properties">
    <xsl:apply-templates/>
  </fo:block>
</xsl:template>

<!-- ============================================================ -->
<!-- Bookmarks -->

<!-- XEP -->

<xsl:template match="slides|foilgroup|foil[not(@role) or @role != 'ENDTITLE']"
              mode="xep.outline">
  <xsl:variable name="id">
    <xsl:call-template name="object.id"/>
  </xsl:variable>
  <xsl:variable name="bookmark-label">
    <xsl:apply-templates select="." mode="object.title.markup"/>
  </xsl:variable>

  <!-- Put the root element bookmark at the same level as its children -->
  <!-- If the object is a set or book, generate a bookmark for the toc -->

  <xsl:choose>
    <xsl:when test="parent::*">
      <rx:bookmark internal-destination="{$id}">
        <rx:bookmark-label>
          <xsl:value-of select="$bookmark-label"/>
        </rx:bookmark-label>
        <xsl:apply-templates select="*" mode="xep.outline"/>
      </rx:bookmark>
    </xsl:when>
    <xsl:otherwise>
      <xsl:if test="$bookmark-label != ''">
        <rx:bookmark internal-destination="{$id}">
          <rx:bookmark-label>
            <xsl:value-of select="$bookmark-label"/>
          </rx:bookmark-label>
        </rx:bookmark>
      </xsl:if>

      <xsl:apply-templates select="*" mode="xep.outline"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- Handling of xrefs -->

<xsl:template match="foil|foilgroup" mode="xref-to">
  <xsl:param name="referrer"/>
  <xsl:param name="xrefstyle"/>
  
  <xsl:apply-templates select="." mode="object.xref.markup">
    <xsl:with-param name="purpose" select="'xref'"/>
    <xsl:with-param name="xrefstyle" select="$xrefstyle"/>
    <xsl:with-param name="referrer" select="$referrer"/>
  </xsl:apply-templates>
</xsl:template>


</xsl:stylesheet>
