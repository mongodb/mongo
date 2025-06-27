<?xml version="1.0" encoding="ASCII"?>
<!--This file was created automatically by html2xhtml-->
<!--from the HTML stylesheets.-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns="http://www.w3.org/1999/xhtml" version="1.0">

<xsl:import href="slides-common.xsl"/>

<xsl:output method="xml" encoding="UTF-8" doctype-public="-//W3C//DTD XHTML 1.0 Transitional//EN" doctype-system="http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd"/>

<xsl:param name="css.stylesheet" select="'slides-default.css'"/>

<xsl:template name="top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <div class="navhead">
    <table width="100%" border="0" cellpadding="0" cellspacing="0" summary="Navigation">
      <tr>
        <xsl:call-template name="generate.toc.hide.show"/>
        <td align="left" width="10%">
          <xsl:choose>
            <xsl:when test="$prev">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$prev" mode="filename"/>
                </xsl:attribute>

                <img alt="Prev" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="prev.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>&#160;</xsl:otherwise>
          </xsl:choose>
        </td>
        <td align="center" width="80%">
          <xsl:variable name="prestitle">
            <xsl:value-of select="(/slides/slidesinfo/title                                   |/slides/title)[1]"/>
          </xsl:variable>

          <span class="navheader">
            <xsl:value-of select="$prestitle"/>
          </span>
        </td>
        <td align="right" width="10%">
          <xsl:choose>
            <xsl:when test="$next">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$next" mode="filename"/>
                </xsl:attribute>

                <img alt="Next" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="next.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>&#160;</xsl:otherwise>
          </xsl:choose>
        </td>
      </tr>
    </table>
    <hr class="top-nav-sep"/>
  </div>
</xsl:template>

<xsl:template name="bottom-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <div class="navfoot">
    <hr class="bottom-nav-sep"/>
    <table width="100%" border="0" cellpadding="0" cellspacing="0" summary="Navigation">
      <tr>
        <td align="left" width="80%" valign="top">
          <span class="navfooter">
            <xsl:apply-templates select="/slides/slidesinfo/copyright" mode="slide.footer.mode"/>
          </span>
        </td>
        <td align="right" width="20%" valign="top">
          <span class="index">
            <xsl:value-of select="count(preceding::foil)                                   + count(preceding::foilgroup)                                   + count(ancestor::foilgroup)                                   + 1"/>
          </span>
          <xsl:text>&#160;</xsl:text>
        </td>
      </tr>
    </table>
  </div>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template name="titlepage-top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <div class="navhead">
    <table width="100%" border="0" cellpadding="0" cellspacing="0" summary="Navigation">
      <tr>
        <td align="left" width="10%">
          <span class="toclink">
            <a href="{$toc.html}">
              <xsl:call-template name="gentext">
                <xsl:with-param name="key">TableofContents</xsl:with-param>
              </xsl:call-template>
            </a>
          </span>
        </td>
        <td align="center" width="80%">
          <xsl:text>&#160;</xsl:text>
        </td>
        <td align="right" width="10%">
          <xsl:text>&#160;</xsl:text>
        </td>
      </tr>
    </table>
  </div>
</xsl:template>

<xsl:template name="titlepage-bottom-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <div class="navfoot">
    <table width="100%" border="0" cellspacing="0" cellpadding="0" summary="Navigation">
      <tr>
        <td align="left" width="80%" valign="top">
          <span class="navfooter">
            <xsl:apply-templates select="/slides/slidesinfo/copyright" mode="slide.footer.mode"/>
          </span>
        </td>
        <td align="right" width="20%" valign="top">
          <a>
            <xsl:attribute name="href">
              <xsl:apply-templates select="(following::foilgroup|following::foil)[1]" mode="filename"/>
            </xsl:attribute>
            <img alt="Next" border="0">
              <xsl:attribute name="src">
                <xsl:call-template name="next.image"/>
              </xsl:attribute>
            </img>
          </a>
        </td>
      </tr>
    </table>
  </div>
</xsl:template>

<xsl:template name="toc-top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <div class="navhead">
    <table width="100%" border="0" cellpadding="0" cellspacing="0" summary="Navigation">
      <tr>
        <td align="left">
          <xsl:text>&#160;</xsl:text>
        </td>
      </tr>
    </table>
  </div>
</xsl:template>

<xsl:template name="toc-bottom-nav">
  <div class="navfoot">
    <table width="100%" border="0" cellspacing="0" cellpadding="0" summary="Navigation">
      <tr>
        <td align="left" width="80%" valign="top">
          <span class="navfooter">
            <xsl:apply-templates select="/slides/slidesinfo/copyright" mode="slide.footer.mode"/>
          </span>
        </td>
        <td align="right" width="20%" valign="top">
          <a href="{$titlefoil.html}">
            <img alt="Next" border="0">
              <xsl:attribute name="src">
                <xsl:call-template name="next.image"/>
              </xsl:attribute>
            </img>
          </a>
        </td>
      </tr>
    </table>
  </div>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template name="generate.toc.hide.show">
  <xsl:if test="$toc.hide.show != 0">
    <td>
      <img hspace="4" alt="Hide/Show TOC">
        <xsl:attribute name="src">
          <xsl:call-template name="hidetoc.image"/>
	</xsl:attribute>
	<xsl:attribute name="onClick">
          <xsl:text>toggletoc(this,</xsl:text>
          <xsl:value-of select="$toc.width"/>
          <xsl:text>,'</xsl:text>
          <xsl:call-template name="hidetoc.image"/>
          <xsl:text>','</xsl:text>
          <xsl:call-template name="showtoc.image"/>
          <xsl:text>');</xsl:text>
        </xsl:attribute>
      </img>
    </td>
  </xsl:if>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template match="@*" mode="copy">
  <xsl:attribute name="{local-name(.)}">
    <xsl:value-of select="."/>
  </xsl:attribute>
</xsl:template>

<xsl:template xmlns:html="http://www.w3.org/1999/xhtml" match="html:*">
  <xsl:element name="{local-name(.)}" namespace="http://www.w3.org/1999/xhtml">
    <xsl:apply-templates select="@*" mode="copy"/>
    <xsl:apply-templates/>
  </xsl:element>
</xsl:template>

<!-- ====================================================================== -->

</xsl:stylesheet>
