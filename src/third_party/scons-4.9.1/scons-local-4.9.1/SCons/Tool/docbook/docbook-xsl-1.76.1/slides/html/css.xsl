<?xml version="1.0"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version="1.0">

<!-- ====================================================================== -->

<xsl:template name="css-file">
  <xsl:param name="css" select="'slides.css'"/>

  <xsl:variable name="source.css.dir">
    <xsl:call-template name="dbhtml-attribute">
      <xsl:with-param name="pis" select="/processing-instruction('dbhtml')"/>
      <xsl:with-param name="attribute" select="'css-stylesheet-dir'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="$source.css.dir != ''">
      <xsl:value-of select="$source.css.dir"/>
      <xsl:text>/</xsl:text>
    </xsl:when>
    <xsl:when test="$css.stylesheet.dir != ''">
      <xsl:value-of select="$css.stylesheet.dir"/>
      <xsl:text>/</xsl:text>
    </xsl:when>
    <xsl:otherwise>
      <xsl:text>http://docbook.sourceforge.net/release/slides/browser/</xsl:text>
    </xsl:otherwise>
  </xsl:choose>
  <xsl:value-of select="$css"/>
</xsl:template>

<!-- ====================================================================== -->
<!-- active navigation images -->

<xsl:template name="css.stylesheet">
  <xsl:param name="css" select="$css.stylesheet"/>
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="css-file">
    <xsl:with-param name="css" select="$css"/>
  </xsl:call-template>
</xsl:template>

<!-- ====================================================================== -->

</xsl:stylesheet>
