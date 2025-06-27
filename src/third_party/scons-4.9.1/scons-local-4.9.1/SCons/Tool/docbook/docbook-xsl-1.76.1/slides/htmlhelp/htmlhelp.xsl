<?xml version="1.0"?>
<!DOCTYPE xsl:stylesheet [
<!ENTITY lf '<xsl:text xmlns:xsl="http://www.w3.org/1999/XSL/Transform">&#xA;</xsl:text>'>
]>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:doc="http://nwalsh.com/xsl/documentation/1.0"
                xmlns:exsl="http://exslt.org/common"
                xmlns:set="http://exslt.org/sets"
		version="1.0"
                exclude-result-prefixes="doc exsl set">

<!-- ********************************************************************
     $Id: htmlhelp.xsl 6569 2007-01-30 07:03:13Z xmldoc $
     ******************************************************************** 

     This stylesheet can convert DocBook Slides document type into HTML Help.

     ******************************************************************** -->

<xsl:import href="../html/plain.xsl"/>
<xsl:include href="../../htmlhelp/htmlhelp-common.xsl"/>

<xsl:param name="keyboard.nav" select="0"/>
<xsl:param name="htmlhelp.default.topic" select="'index.html'"/>

<xsl:template match="slides" mode="title.markup">
  <xsl:param name="allow-anchors" select="0"/>
  <xsl:apply-templates select="(slidesinfo/title|title)[1]"
                       mode="title.markup">
    <xsl:with-param name="allow-anchors" select="$allow-anchors"/>
  </xsl:apply-templates>
</xsl:template>

<xsl:template match="slides|foilgroup" mode="hhc">
  <xsl:variable name="title">
    <xsl:if test="$htmlhelp.autolabel=1">
      <xsl:variable name="label.markup">
        <xsl:apply-templates select="." mode="label.markup"/>
      </xsl:variable>
      <xsl:if test="normalize-space($label.markup)">
        <xsl:value-of select="concat($label.markup,$autotoc.label.separator)"/>
      </xsl:if>
    </xsl:if>
    <xsl:apply-templates select="." mode="title.markup"/>
  </xsl:variable>

  <xsl:if test="$htmlhelp.hhc.show.root != 0 or parent::*">
    <LI><OBJECT type="text/sitemap">&lf;
      <param name="Name" value="{normalize-space($title)}"/>&lf;
      <param name="Local">
	<xsl:attribute name="value">
          <xsl:apply-templates select="." mode="filename"/>
	</xsl:attribute>
      </param>
    </OBJECT></LI>&lf;
  </xsl:if>
  <xsl:if test="foil|foilgroup">
    <UL>&lf;
      <xsl:apply-templates select="foil|foilgroup" mode="hhc"/>
    </UL>&lf;
  </xsl:if>
</xsl:template>

<xsl:template match="foil" mode="hhc">
  <xsl:variable name="title">
    <xsl:if test="$htmlhelp.autolabel=1">
      <xsl:variable name="label.markup">
        <xsl:apply-templates select="." mode="label.markup"/>
      </xsl:variable>
      <xsl:if test="normalize-space($label.markup)">
        <xsl:value-of select="concat($label.markup,$autotoc.label.separator)"/>
      </xsl:if>
    </xsl:if>
    <xsl:apply-templates select="." mode="title.markup"/>
  </xsl:variable>

  <xsl:if test="$htmlhelp.hhc.show.root != 0 or parent::*">
    <LI><OBJECT type="text/sitemap">&lf;
      <param name="Name" value="{normalize-space($title)}"/>&lf;
      <param name="Local">
	<xsl:attribute name="value">
	  <xsl:apply-templates select="." mode="filename"/>
	</xsl:attribute>
      </param>
    </OBJECT></LI>&lf;
  </xsl:if>
</xsl:template>

</xsl:stylesheet>
