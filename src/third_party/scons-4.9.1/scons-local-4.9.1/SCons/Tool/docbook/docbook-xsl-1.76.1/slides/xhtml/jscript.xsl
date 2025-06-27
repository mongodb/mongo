<?xml version="1.0" encoding="ASCII"?>
<!--This file was created automatically by html2xhtml-->
<!--from the HTML stylesheets.-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns="http://www.w3.org/1999/xhtml" version="1.0">

<xsl:output method="xml" encoding="UTF-8" doctype-public="-//W3C//DTD XHTML 1.0 Transitional//EN" doctype-system="http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd"/>

<xsl:template name="script-dir">
  <xsl:variable name="source.script.dir">
    <xsl:call-template name="dbhtml-attribute">
      <xsl:with-param name="pis" select="/processing-instruction('dbhtml')"/>
      <xsl:with-param name="attribute" select="'script-dir'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="$source.script.dir != ''">
      <xsl:value-of select="$source.script.dir"/>
      <xsl:text>/</xsl:text>
    </xsl:when>
    <xsl:when test="$script.dir != ''">
      <xsl:value-of select="$script.dir"/>
      <xsl:text>/</xsl:text>
    </xsl:when>
    <xsl:otherwise>
      <xsl:text>http://docbook.sourceforge.net/release/slides/browser/</xsl:text>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template name="script-file">
  <xsl:param name="js" select="'slides.js'"/>
  <xsl:call-template name="script-dir"/>
  <xsl:value-of select="$js"/>
</xsl:template>

<xsl:template name="ua.js">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:param name="language" select="'javascript'"/>
  <script type="text/javascript" language="{$language}">
    <xsl:attribute name="src">
      <xsl:call-template name="script-file">
        <xsl:with-param name="js" select="$ua.js"/>
      </xsl:call-template>
    </xsl:attribute>
    <xsl:text> </xsl:text>
  </script>
</xsl:template>

<xsl:template name="xbDOM.js">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:param name="language" select="'javascript'"/>
  <script type="text/javascript" language="{$language}">
    <xsl:attribute name="src">
      <xsl:call-template name="script-file">
        <xsl:with-param name="js" select="$xbDOM.js"/>
      </xsl:call-template>
    </xsl:attribute>
    <xsl:text> </xsl:text>
  </script>
</xsl:template>

<xsl:template name="xbStyle.js">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:param name="language" select="'javascript'"/>
  <script type="text/javascript" language="{$language}">
    <xsl:attribute name="src">
      <xsl:call-template name="script-file">
        <xsl:with-param name="js" select="$xbStyle.js"/>
      </xsl:call-template>
    </xsl:attribute>
    <xsl:text> </xsl:text>
  </script>
</xsl:template>

<xsl:template name="xbLibrary.js">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:param name="language" select="'javascript'"/>
  <script type="text/javascript" language="{$language}">
    <xsl:attribute name="src">
      <xsl:call-template name="script-file">
        <xsl:with-param name="js" select="$xbLibrary.js"/>
      </xsl:call-template>
    </xsl:attribute>
    <xsl:text> </xsl:text>
  </script>
</xsl:template>

<xsl:template name="xbCollapsibleLists.js">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:param name="language" select="'javascript'"/>
  <script type="text/javascript" language="{$language}">
    <xsl:attribute name="src">
      <xsl:call-template name="script-file">
        <xsl:with-param name="js" select="$xbCollapsibleLists.js"/>
      </xsl:call-template>
    </xsl:attribute>
    <xsl:text> </xsl:text>
  </script>
</xsl:template>

<xsl:template name="overlay.js">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:param name="language" select="'javascript'"/>
  <script type="text/javascript" language="{$language}">
    <xsl:attribute name="src">
      <xsl:call-template name="script-file">
        <xsl:with-param name="js" select="$overlay.js"/>
      </xsl:call-template>
    </xsl:attribute>
    <xsl:text> </xsl:text>
  </script>
</xsl:template>

<xsl:template name="slides.js">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:param name="language" select="'javascript'"/>
  <script type="text/javascript" language="{$language}">
    <xsl:attribute name="src">
      <xsl:call-template name="script-file">
        <xsl:with-param name="js" select="$slides.js"/>
      </xsl:call-template>
    </xsl:attribute>
    <xsl:text> </xsl:text>
  </script>
</xsl:template>

</xsl:stylesheet>
