<?xml version="1.0" encoding="ASCII"?>
<!--This file was created automatically by html2xhtml-->
<!--from the HTML stylesheets.-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns="http://www.w3.org/1999/xhtml" version="1.0">

<xsl:import href="slides-common.xsl"/>

<xsl:param name="prev.image" select="'active/nav-prev.png'"/>
<xsl:param name="next.image" select="'active/nav-next.png'"/>
<xsl:param name="up.image" select="'active/nav-up.png'"/>
<xsl:param name="toc.image" select="'active/nav-toc.png'"/>
<xsl:param name="home.image" select="'active/nav-home.png'"/>

<xsl:param name="no.prev.image" select="'inactive/nav-prev.png'"/>
<xsl:param name="no.next.image" select="'inactive/nav-next.png'"/>
<xsl:param name="no.up.image" select="'inactive/nav-up.png'"/>
<xsl:param name="no.toc.image" select="'inactive/nav-toc.png'"/>
<xsl:param name="no.home.image" select="'inactive/nav-home.png'"/>

<xsl:param name="css.stylesheet" select="'slides-plain.css'"/>

<!-- ====================================================================== -->

<xsl:template name="top-nav">
  <xsl:param name="prev"/>
  <xsl:param name="next"/>
  <xsl:param name="up"/>
  <xsl:param name="home"/>
  <xsl:param name="toc" select="$toc.html"/>

  <div class="navhead">
    <table width="100%" border="0" cellpadding="0" cellspacing="0" summary="Navigation">
      <tr>
        <td align="left" width="45%">
          <span class="slidestitle">
            <xsl:value-of select="(/slides/slidesinfo/title)[1]"/>
          </span>
          <xsl:text>&#160;</xsl:text>
        </td>
	<td width="10%" align="center" valign="bottom">
          <xsl:call-template name="foil.number"/>
        </td>
        <td align="right" width="45%">
          <xsl:choose>
            <xsl:when test="$home">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$home" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$home/slidesinfo/title"/>
                </xsl:attribute>
                <img alt="Home" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="home.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Home" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.home.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$toc != ''">
              <a title="ToC" href="{$toc}">
                <img alt="ToC" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="toc.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="ToC" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.toc.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$prev">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$prev" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$prev/title"/>
                </xsl:attribute>
                <img alt="Prev" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="prev.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Prev" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.prev.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$up">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$up" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$up/title"/>
                </xsl:attribute>
                <img alt="Up" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="up.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Up" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.up.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$next">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$next" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$next/title"/>
                </xsl:attribute>
                <img alt="Next" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="next.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Next" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.next.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>
        </td>
      </tr>
    </table>
    <hr class="top-nav-sep"/>
  </div>
</xsl:template>

<!-- ============================================================ -->

<xsl:template name="titlepage-top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="prev"/>
  <xsl:param name="next"/>
  <xsl:param name="toc" select="$toc.html"/>

  <div class="navhead">
    <table width="100%" border="0" cellpadding="0" cellspacing="0" summary="Navigation">
      <tr>
        <td align="left" width="50%">
          <xsl:text>&#160;</xsl:text>
        </td>
        <td align="right" width="50%">
          <xsl:choose>
            <xsl:when test="$home">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$home" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$home/slidesinfo/title"/>
                </xsl:attribute>
                <img alt="Home" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="home.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Home" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.home.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$toc.html != ''">
              <a title="ToC" href="{$toc.html}">
                <img alt="ToC" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="toc.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="ToC" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.toc.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$prev">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$prev" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$prev/title"/>
                </xsl:attribute>
                <img alt="Prev" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="prev.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Prev" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.prev.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$up">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$up" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$up/title"/>
                </xsl:attribute>
                <img alt="Up" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="up.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Up" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.up.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$next">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$next" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$next/title"/>
                </xsl:attribute>
                <img alt="Next" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="next.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Next" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.next.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>
        </td>
      </tr>
    </table>
    <hr class="top-nav-sep"/>
  </div>
</xsl:template>

<!-- ============================================================ -->

<xsl:template name="toc-top-nav">
  <xsl:param name="home" select="/slides"/>
  <xsl:param name="up"/>
  <xsl:param name="prev"/>
  <xsl:param name="next" select="(foil|foilgroup)[1]"/>
  <xsl:param name="toc"/>

  <div class="navhead">
    <table width="100%" border="0" cellpadding="0" cellspacing="0" summary="Navigation">
      <tr>
        <td align="left" width="50%">
          <xsl:text>&#160;</xsl:text>
        </td>
        <td align="right" width="50%">
          <xsl:choose>
            <xsl:when test="$home">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$home" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$home/slidesinfo/title"/>
                </xsl:attribute>
                <img alt="Home" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="home.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Home" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.home.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$toc != ''">
              <a title="ToC" href="{$toc}">
                <img alt="ToC" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="toc.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="ToC" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.toc.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$prev">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$prev" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$prev/title"/>
                </xsl:attribute>
                <img alt="Prev" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="prev.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Prev" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.prev.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$up">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$up" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$up/title"/>
                </xsl:attribute>
                <img alt="Up" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="up.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Up" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.up.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$next">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$next" mode="filename"/>
                </xsl:attribute>
                <xsl:attribute name="title">
                  <xsl:value-of select="$next/title"/>
                </xsl:attribute>
                <img alt="Next" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="next.image"/>
                  </xsl:attribute>
                </img>
              </a>
            </xsl:when>
            <xsl:otherwise>
              <img alt="Next" border="0">
                <xsl:attribute name="src">
                  <xsl:call-template name="no.next.image"/>
                </xsl:attribute>
              </img>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>
        </td>
      </tr>
    </table>
    <hr class="top-nav-sep"/>
  </div>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template name="bottom-nav"/>

</xsl:stylesheet>
