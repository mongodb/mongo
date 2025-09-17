<?xml version="1.0"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version="1.0">

<!-- ====================================================================== -->

<xsl:template name="graphics-file">
  <xsl:param name="image" select="'bullet.gif'"/>

  <xsl:variable name="source.graphics.dir">
    <xsl:call-template name="dbhtml-attribute">
      <xsl:with-param name="pis" select="/processing-instruction('dbhtml')"/>
      <xsl:with-param name="attribute" select="'graphics-dir'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="$source.graphics.dir != ''">
      <xsl:value-of select="$source.graphics.dir"/>
      <xsl:text>/</xsl:text>
    </xsl:when>
    <xsl:when test="$graphics.dir != ''">
      <xsl:value-of select="$graphics.dir"/>
      <xsl:text>/</xsl:text>
    </xsl:when>
    <xsl:otherwise>
      <xsl:text>http://docbook.sourceforge.net/release/slides/graphics/</xsl:text>
    </xsl:otherwise>
  </xsl:choose>
  <xsl:value-of select="$image"/>
</xsl:template>

<!-- ====================================================================== -->
<!-- active navigation images -->

<xsl:template name="toc.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$toc.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="home.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$home.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="up.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$up.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="prev.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$prev.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="next.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$next.image"/>
  </xsl:call-template>
</xsl:template>

<!-- inactive navigation images -->

<xsl:template name="no.toc.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$no.toc.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="no.home.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$no.home.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="no.up.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$no.up.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="no.prev.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$no.prev.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="no.next.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$no.next.image"/>
  </xsl:call-template>
</xsl:template>

<!-- ====================================================================== -->
<!-- icon images -->

<xsl:template name="bullet.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$bullet.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="plus.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$plus.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="minus.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$minus.image"/>
  </xsl:call-template>
</xsl:template>

<!-- ====================================================================== -->
<!-- hide/show ToC images -->

<xsl:template name="hidetoc.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$hidetoc.image"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="showtoc.image">
  <!-- Danger Will Robinson: template shadows parameter -->
  <xsl:call-template name="graphics-file">
    <xsl:with-param name="image" select="$showtoc.image"/>
  </xsl:call-template>
</xsl:template>

<!-- ====================================================================== -->

</xsl:stylesheet>
