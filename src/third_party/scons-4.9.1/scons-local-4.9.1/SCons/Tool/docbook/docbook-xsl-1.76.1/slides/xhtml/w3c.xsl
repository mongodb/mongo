<?xml version="1.0" encoding="ASCII"?>
<!--This file was created automatically by html2xhtml-->
<!--from the HTML stylesheets.-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns="http://www.w3.org/1999/xhtml" version="1.0">

<xsl:import href="slides-common.xsl"/>

<xsl:param name="logo.image.uri" select="''"/>
<xsl:param name="logo.uri" select="''"/>
<xsl:param name="logo.title" select="''"/>

<xsl:param name="next.image" select="'active/w3c-next.png'"/>
<xsl:param name="no.next.image" select="'inactive/w3c-next.png'"/>

<xsl:param name="prev.image" select="'active/w3c-prev.png'"/>
<xsl:param name="no.prev.image" select="'inactive/w3c-prev.png'"/>

<xsl:param name="toc.image" select="'active/w3c-toc.png'"/>
<xsl:param name="no.toc.image" select="'inactive/w3c-toc.png'"/>

<xsl:param name="css.stylesheet" select="'slides-w3c.css'"/>

<xsl:template name="logo">
  <xsl:if test="$logo.uri != ''">
    <a href="{$logo.uri}" title="{$logo.title}">
      <xsl:choose>
        <xsl:when test="$logo.image.uri=''">
          <xsl:value-of select="$logo.title"/>
        </xsl:when>
        <xsl:otherwise>
          <img src="{$logo.image.uri}" alt="{$logo.title}" border="0"/>
        </xsl:otherwise>
      </xsl:choose>
    </a>
  </xsl:if>
</xsl:template>

<xsl:template name="overlayDiv.attributes">
  <xsl:if test="$overlay != 0">
    <xsl:attribute name="style">
      <xsl:text>position: absolute; visibility: visible;</xsl:text>
    </xsl:attribute>
  </xsl:if>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template name="top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <div class="navhead">
    <table class="navbar" cellspacing="0" cellpadding="0" border="0" width="97%" summary="Navigation buttons">
      <tr>
        <td align="left" valign="top">
          <xsl:call-template name="logo"/>
        </td>
        <td valign="top" nowrap="nowrap" width="150">
          <div align="right">
            <xsl:choose>
              <xsl:when test="$prev">
                <a rel="previous" accesskey="P">
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$prev" mode="filename"/>
                  </xsl:attribute>
                  <img border="0" width="32" height="32" alt=" Previous" title="{$prev/title}">
                    <xsl:attribute name="src">
                      <xsl:call-template name="prev.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <img border="0" width="32" height="32" alt=" Previous" title="">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.prev.image"/>
                  </xsl:attribute>
                </img>
              </xsl:otherwise>
            </xsl:choose>

            <xsl:choose>
              <xsl:when test="$tocfile != ''">
                <a rel="contents" href="{$tocfile}" accesskey="C">
                  <img border="0" width="32" height="32" alt=" Contents" title="Table of Contents">
                    <xsl:attribute name="src">
                      <xsl:call-template name="toc.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <img border="0" width="32" height="32" alt=" Contents" title="Table of Contents">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.toc.image"/>
                  </xsl:attribute>
                </img>
              </xsl:otherwise>
            </xsl:choose>

            <xsl:choose>
              <xsl:when test="$next">
                <a rel="next" accesskey="N">
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$next" mode="filename"/>
                  </xsl:attribute>
                  <img border="0" width="32" height="32" alt=" Next" title="{$next/title}">
                    <xsl:attribute name="src">
                      <xsl:call-template name="next.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <img border="0" width="32" height="32" alt=" Next" title="">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.next.image"/>
                  </xsl:attribute>
                </img>
              </xsl:otherwise>
            </xsl:choose>
          </div>
        </td>
      </tr>
    </table>

    <xsl:apply-templates select="title"/>
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
    <table class="footer" cellspacing="0" cellpadding="0" border="0" width="97%" summary="footer">
      <tr>
        <td align="left" valign="top">
          <xsl:variable name="author" select="(/slides/slidesinfo//author                                               |/slides/slidesinfo//editor)"/>
          <xsl:for-each select="$author">
            <xsl:choose>
              <xsl:when test=".//email">
                <a href="mailto:{.//email[1]}">
                  <xsl:call-template name="person.name"/>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <xsl:call-template name="person.name"/>
              </xsl:otherwise>
            </xsl:choose>

            <xsl:if test="position() &lt; last()">, </xsl:if>
          </xsl:for-each>
        </td>
        <td align="right" valign="top">
          <span class="index">
            <xsl:value-of select="count(preceding::foil)                                   + count(preceding::foilgroup)                                   + count(ancestor::foilgroup)                                   + 1"/>
            <xsl:text> of </xsl:text>
            <xsl:value-of select="count(//foil|//foilgroup)"/>
          </span>
        </td>

        <td valign="top" nowrap="nowrap" width="150">
          <div align="right">
            <xsl:choose>
              <xsl:when test="$prev">
                <a rel="previous" accesskey="P">
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$prev" mode="filename"/>
                  </xsl:attribute>
                  <img border="0" width="32" height="32" alt=" Previous" title="{$prev/title}">
                    <xsl:attribute name="src">
                      <xsl:call-template name="prev.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <img border="0" width="32" height="32" alt=" Prev" title="">
                <xsl:attribute name="src">
                    <xsl:call-template name="no.prev.image"/>
                </xsl:attribute>
                </img>
              </xsl:otherwise>
            </xsl:choose>

            <xsl:choose>
              <xsl:when test="$next">
                <a rel="next" accesskey="N">
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$next" mode="filename"/>
                  </xsl:attribute>
                  <img border="0" width="32" height="32" alt=" Next" title="{$next/title}">
                    <xsl:attribute name="src">
                      <xsl:call-template name="next.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <img border="0" width="32" height="32" alt=" Next" title="">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.next.image"/>
                  </xsl:attribute>
                </img>
              </xsl:otherwise>
            </xsl:choose>
          </div>
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
    <table class="navbar" cellspacing="0" cellpadding="0" border="0" width="97%" summary="Navigation buttons">
      <tr>
        <td align="left" valign="top">
          <xsl:call-template name="logo"/>
        </td>
        <td valign="top" nowrap="nowrap" width="150">
          <div align="right">
            <xsl:choose>
              <xsl:when test="$prev">
                <a rel="previous" accesskey="P">
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$prev" mode="filename"/>
                  </xsl:attribute>
                  <img border="0" width="32" height="32" alt=" Previous" title="{$prev/title}">
                    <xsl:attribute name="src">
                      <xsl:call-template name="prev.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <img border="0" width="32" height="32" alt=" Previous" title="">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.prev.image"/>
                  </xsl:attribute>
                </img>
              </xsl:otherwise>
            </xsl:choose>

            <xsl:choose>
              <xsl:when test="$tocfile != ''">
                <a rel="contents" href="{$tocfile}" accesskey="C">
                  <img border="0" width="32" height="32" alt=" Contents" title="Table of Contents">
                    <xsl:attribute name="src">
                      <xsl:call-template name="toc.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <img border="0" width="32" height="32" alt=" Contents" title="Table of Contents">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.toc.image"/>
                  </xsl:attribute>
                </img>
              </xsl:otherwise>
            </xsl:choose>

            <xsl:choose>
              <xsl:when test="$next">
                <a rel="next" accesskey="N">
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$next" mode="filename"/>
                  </xsl:attribute>
                  <img border="0" width="32" height="32" alt=" Next" title="{$next/title}">
                    <xsl:attribute name="src">
                      <xsl:call-template name="next.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </xsl:when>
              <xsl:otherwise>
                <img border="0" width="32" height="32" alt=" Next" title="">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.next.image"/>
                  </xsl:attribute>
                </img>
              </xsl:otherwise>
            </xsl:choose>
          </div>
        </td>
      </tr>
    </table>

    <hr class="top-nav-sep"/>
  </div>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template name="foil-body">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <!-- skip the title -->
  <xsl:apply-templates select="*[name(.) != 'title']"/>
</xsl:template>

<xsl:template name="foilgroup-body">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <!-- skip the title -->
  <xsl:apply-templates select="*[name(.) != 'title'                                 and name(.) != 'foil'                                 and name(.) != 'foilgroup']"/>

  <xsl:if test="$foilgroup.toc != 0">
    <dl class="toc">
      <xsl:apply-templates select="foil" mode="toc"/>
    </dl>
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
