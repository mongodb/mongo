<?xml version="1.0" encoding="ASCII"?>
<!--This file was created automatically by html2xhtml-->
<!--from the HTML stylesheets.-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns="http://www.w3.org/1999/xhtml" version="1.0">

<xsl:import href="../../xhtml/chunk.xsl"/>
<xsl:include href="../../VERSION"/>

<xsl:include href="param.xsl"/>
<xsl:include href="jscript.xsl"/>
<xsl:include href="graphics.xsl"/>
<xsl:include href="css.xsl"/>

<xsl:output method="xml" encoding="UTF-8" doctype-public="-//W3C//DTD XHTML 1.0 Transitional//EN" doctype-system="http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd"/>

<xsl:strip-space elements="slides foil foilgroup"/>

<!-- Process the slides -->

<xsl:template match="/">
  <xsl:apply-templates/>
</xsl:template>

<xsl:template match="slides">
  <xsl:apply-templates select="." mode="toc"/>
  <xsl:apply-templates/>
</xsl:template>

<!-- ====================================================================== -->
<!-- Every slide has top and bottom navigation -->

<xsl:template name="top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <div class="navhead">
    <table border="0" width="100%" cellspacing="0" cellpadding="0" summary="Navigation table">
      <tr>
        <td align="left" valign="bottom">
          <xsl:if test="$home">
            <span class="slidestitle">
              <a>
                <xsl:attribute name="href">
                  <xsl:apply-templates select="$home" mode="filename"/>
                </xsl:attribute>
                <xsl:value-of select="($home/title|$home/slidesinfo/title)[1]"/>
              </a>
            </span>
          </xsl:if>
          <xsl:text>&#160;</xsl:text>
        </td>

        <td align="right" valign="bottom">
          <xsl:choose>
            <xsl:when test="$home">
              <span class="link-text">
                <a>
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$home" mode="filename"/>
                  </xsl:attribute>
                  <img alt="{$text.home}" border="0">
                    <xsl:attribute name="src">
                      <xsl:call-template name="home.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </span>
            </xsl:when>
            <xsl:otherwise>
              <span class="no-link-text">
                <img alt="{$text.home}" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.home.image"/>
                  </xsl:attribute>
                </img>
              </span>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$tocfile">
              <span class="link-text">
                <a>
                  <xsl:attribute name="href">
                    <xsl:value-of select="$tocfile"/>
                  </xsl:attribute>
                  <img alt="{$text.toc}" border="0">
                    <xsl:attribute name="src">
                      <xsl:call-template name="toc.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </span>
            </xsl:when>
            <xsl:otherwise>
              <span class="no-link-text">
                <img alt="{$text.toc}" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.toc.image"/>
                  </xsl:attribute>
                </img>
              </span>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$prev">
              <span class="link-text">
                <a>
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$prev" mode="filename"/>
                  </xsl:attribute>
                  <img alt="{$text.prev}" border="0">
                    <xsl:attribute name="src">
                      <xsl:call-template name="prev.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </span>
            </xsl:when>
            <xsl:otherwise>
              <span class="no-link-text">
                <img alt="{$text.prev}" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.prev.image"/>
                  </xsl:attribute>
                </img>
              </span>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$up">
              <span class="link-text">
                <a>
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$up" mode="filename"/>
                  </xsl:attribute>
                  <img alt="{$text.up}" border="0">
                    <xsl:attribute name="src">
                      <xsl:call-template name="up.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </span>
            </xsl:when>
            <xsl:otherwise>
              <span class="no-link-text">
                <img alt="{$text.up}" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.up.image"/>
                  </xsl:attribute>
                </img>
              </span>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$next">
              <span class="link-text">
                <a>
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$next" mode="filename"/>
                  </xsl:attribute>
                  <img alt="{$text.next}" border="0">
                    <xsl:attribute name="src">
                      <xsl:call-template name="next.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </span>
            </xsl:when>
            <xsl:otherwise>
              <span class="no-link-text">
                <img alt="{$text.next}" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.next.image"/>
                  </xsl:attribute>
                </img>
              </span>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>
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
    <table border="0" width="100%" cellspacing="0" cellpadding="0" summary="Navigation table">
      <tr>
        <td align="left" valign="top">
          <xsl:apply-templates select="/slides/slidesinfo/copyright" mode="slide.footer.mode"/>
          <xsl:text>&#160;</xsl:text>
        </td>

        <td align="right" valign="top">
          <xsl:choose>
            <xsl:when test="$prev">
              <span class="link-text">
                <a>
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$prev" mode="filename"/>
                  </xsl:attribute>
                  <img alt="{$text.prev}" border="0">
                    <xsl:attribute name="src">
                      <xsl:call-template name="prev.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </span>
            </xsl:when>
            <xsl:otherwise>
              <span class="no-link-text">
                <img alt="{$text.prev}" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.prev.image"/>
                  </xsl:attribute>
                </img>
              </span>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>

          <xsl:choose>
            <xsl:when test="$next">
              <span class="link-text">
                <a>
                  <xsl:attribute name="href">
                    <xsl:apply-templates select="$next" mode="filename"/>
                  </xsl:attribute>
                  <img alt="{$text.next}" border="0">
                    <xsl:attribute name="src">
                      <xsl:call-template name="next.image"/>
                    </xsl:attribute>
                  </img>
                </a>
              </span>
            </xsl:when>
            <xsl:otherwise>
              <span class="no-link-text">
                <img alt="{$text.next}" border="0">
                  <xsl:attribute name="src">
                    <xsl:call-template name="no.next.image"/>
                  </xsl:attribute>
                </img>
              </span>
            </xsl:otherwise>
          </xsl:choose>
          <xsl:text>&#160;</xsl:text>
        </td>
      </tr>
    </table>
  </div>
</xsl:template>

<!-- Navigation is also provided in the form of links in the head -->

<xsl:template name="links">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <xsl:if test="$tocfile != ''">
    <link rel="contents" href="{$tocfile}">
      <xsl:attribute name="title">
        <xsl:value-of select="/slides/slidesinfo/title"/>
      </xsl:attribute>
    </link>
  </xsl:if>

  <xsl:if test="$home">
    <link rel="top">
      <xsl:attribute name="href">
        <xsl:apply-templates select="$home" mode="filename"/>
      </xsl:attribute>
      <xsl:attribute name="title">
        <xsl:value-of select="($home/title|$home/slidesinfo/title)[1]"/>
      </xsl:attribute>
    </link>

    <link rel="first">
      <xsl:attribute name="href">
        <xsl:apply-templates select="$home" mode="filename"/>
      </xsl:attribute>
      <xsl:attribute name="title">
        <xsl:value-of select="($home/title|$home/slidesinfo/title)[1]"/>
      </xsl:attribute>
    </link>
  </xsl:if>

  <xsl:if test="$up">
    <link rel="up">
      <xsl:attribute name="href">
        <xsl:apply-templates select="$up" mode="filename"/>
      </xsl:attribute>
      <xsl:attribute name="title">
        <xsl:value-of select="($up/title|$up/slidesinfo/title)[1]"/>
      </xsl:attribute>
    </link>
  </xsl:if>

  <xsl:if test="$prev">
    <link rel="previous">
      <xsl:attribute name="href">
        <xsl:apply-templates select="$prev" mode="filename"/>
      </xsl:attribute>
      <xsl:attribute name="title">
        <xsl:value-of select="($prev/title|$prev/slidesinfo/title)[1]"/>
      </xsl:attribute>
    </link>
  </xsl:if>

  <xsl:if test="$next">
    <link rel="next">
      <xsl:attribute name="href">
        <xsl:apply-templates select="$next" mode="filename"/>
      </xsl:attribute>
      <xsl:attribute name="title">
        <xsl:value-of select="$next/title"/>
      </xsl:attribute>
    </link>

    <xsl:variable name="last" select="$next/following::foil[last()]"/>
    <xsl:if test="$last">
      <link rel="last">
        <xsl:attribute name="href">
          <xsl:apply-templates select="$last" mode="filename"/>
        </xsl:attribute>
        <xsl:attribute name="title">
          <xsl:value-of select="$last/title"/>
        </xsl:attribute>
      </link>
    </xsl:if>
  </xsl:if>

  <xsl:for-each select="foil">
    <link rel="slides">
      <xsl:attribute name="href">
        <xsl:apply-templates select="." mode="filename"/>
      </xsl:attribute>
      <xsl:attribute name="title">
        <xsl:value-of select="title[1]"/>
      </xsl:attribute>
    </link>
  </xsl:for-each>

  <xsl:for-each select="foilgroup|../foilgroup">
    <link rel="section">
      <xsl:attribute name="href">
        <xsl:apply-templates select="." mode="filename"/>
      </xsl:attribute>
      <xsl:attribute name="title">
        <xsl:value-of select="title[1]"/>
      </xsl:attribute>
    </link>
  </xsl:for-each>
</xsl:template>

<!-- ====================================================================== -->
<!-- There are four kinds of slides: titlepage, toc, foil, and foilgroup -->
<!-- titlepage -->

<xsl:template match="slidesinfo">
  <xsl:variable name="id">
    <xsl:call-template name="object.id"/>
  </xsl:variable>

  <xsl:variable name="next" select="(/slides/foil|/slides/foilgroup)[1]"/>
  <xsl:variable name="tocfile" select="$toc.html"/>
  <xsl:variable name="dir">
    <xsl:call-template name="dbhtml-dir"/>
  </xsl:variable>


  <xsl:call-template name="write.chunk">
    <xsl:with-param name="indent" select="$output.indent"/>
    <xsl:with-param name="filename" select="concat($base.dir, $dir, $titlefoil.html)"/>
    <xsl:with-param name="content">
      <html>
        <head>
          <title><xsl:value-of select="title"/></title>

          <xsl:call-template name="system.head.content">
            <xsl:with-param name="node" select=".."/>
          </xsl:call-template>
	  
	  <meta name="generator" content="DocBook Slides Stylesheets V{$VERSION}"/>

          <!-- Links -->
          <xsl:if test="$css.stylesheet != ''">
            <link type="text/css" rel="stylesheet">
              <xsl:attribute name="href">
                <xsl:call-template name="css.stylesheet"/>
              </xsl:attribute>
            </link>
          </xsl:if>
          <xsl:apply-templates select="/processing-instruction('dbhtml')" mode="css.pi"/>

          <xsl:call-template name="links">
            <xsl:with-param name="home" select="/slides"/>
            <xsl:with-param name="next" select="$next"/>
            <xsl:with-param name="tocfile" select="$tocfile"/>
          </xsl:call-template>

          <!-- Scripts -->

          <xsl:if test="$overlay != 0 or $keyboard.nav != 0">
            <script language="javascript" type="text/javascript">
              <xsl:text> </xsl:text>
            </script>
          </xsl:if>

          <xsl:if test="$keyboard.nav != 0">
            <xsl:call-template name="ua.js"/>
            <xsl:call-template name="xbDOM.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
            <xsl:call-template name="xbLibrary.js"/>
            <script language="javascript" type="text/javascript">
              <xsl:text disable-output-escaping="yes">
                &lt;!--
                xblibrary = new xbLibrary('</xsl:text>
              <xsl:call-template name="script-dir"/>
              <xsl:text disable-output-escaping="yes">');
                // --&gt;
              </xsl:text>
            </script>
            <xsl:call-template name="xbStyle.js"/>
            <xsl:call-template name="xbCollapsibleLists.js"/>
            <xsl:call-template name="slides.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>

          <xsl:if test="$overlay != '0'">
            <xsl:call-template name="overlay.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>

          <xsl:call-template name="user.head.content">
            <xsl:with-param name="node" select=".."/>
          </xsl:call-template>
        </head>
        <body>
          <xsl:attribute name="class">
            <xsl:text>titlepage</xsl:text>
            <xsl:if test="@role">
              <xsl:text>-</xsl:text>
              <xsl:value-of select="@role"/>
            </xsl:if>
          </xsl:attribute>

          <xsl:call-template name="body.attributes"/>
          <xsl:if test="$overlay != 0">
            <xsl:attribute name="onload">
              <xsl:text>overlaySetup('lc')</xsl:text>
            </xsl:attribute>
          </xsl:if>
          <xsl:if test="$keyboard.nav != 0">
            <xsl:attribute name="onkeypress">
              <xsl:text>navigate(event)</xsl:text>
            </xsl:attribute>
          </xsl:if>

          <div class="titlepage" id="{$id}">
            <xsl:call-template name="titlepage-top-nav">
              <xsl:with-param name="next" select="$next"/>
              <xsl:with-param name="tocfile" select="$tocfile"/>
            </xsl:call-template>

            <div class="titlepage-body">
              <xsl:call-template name="titlepage-body"/>
            </div>

            <div id="overlayDiv">
              <xsl:call-template name="overlayDiv.attributes"/>
              <xsl:call-template name="titlepage-bottom-nav">
                <xsl:with-param name="next" select="$next"/>
                <xsl:with-param name="tocfile" select="$tocfile"/>
              </xsl:call-template>
            </div>
          </div>
        </body>
      </html>
    </xsl:with-param>
  </xsl:call-template>
</xsl:template>

<xsl:template name="titlepage-body">
  <div class="{name(.)}">
    <xsl:apply-templates mode="titlepage.mode"/>
  </div>
</xsl:template>

<xsl:template name="titlepage-top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <xsl:call-template name="top-nav">
    <xsl:with-param name="home" select="$home"/>
    <xsl:with-param name="up" select="$up"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="prev" select="$prev"/>
    <xsl:with-param name="tocfile" select="$tocfile"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="titlepage-bottom-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <xsl:call-template name="bottom-nav">
    <xsl:with-param name="home" select="$home"/>
    <xsl:with-param name="up" select="$up"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="prev" select="$prev"/>
    <xsl:with-param name="tocfile" select="$tocfile"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="slidesinfo/title">
  <h1 class="{name(.)}"><xsl:apply-templates/></h1>
</xsl:template>

<xsl:template match="slidesinfo/authorgroup">
  <xsl:apply-templates/>
</xsl:template>

<xsl:template match="slidesinfo/author|slidesinfo/authorgroup/author">
  <h1 class="{name(.)}"><xsl:apply-imports/></h1>
</xsl:template>

<xsl:template match="slidesinfo/releaseinfo">
  <h4 class="{name(.)}"><xsl:apply-templates/></h4>
</xsl:template>

<xsl:template match="slidesinfo/date">
  <h4 class="{name(.)}"><xsl:apply-templates/></h4>
</xsl:template>

<xsl:template match="slidesinfo/copyright">
  <!-- nop -->
</xsl:template>

<!-- On slides, output the credits explicitly each time -->
<xsl:template match="othercredit" mode="titlepage.mode">
  <xsl:variable name="contrib" select="string(contrib)"/>
  <xsl:choose>
    <xsl:when test="contrib">
      <xsl:call-template name="paragraph">
	<xsl:with-param name="class" select="name(.)"/>
	<xsl:with-param name="content">
	  <xsl:apply-templates mode="titlepage.mode" select="contrib"/>
	  <xsl:text>: </xsl:text>
	  <xsl:call-template name="person.name"/>
	  <xsl:apply-templates mode="titlepage.mode" select="./affiliation"/>
	</xsl:with-param>
      </xsl:call-template>
    </xsl:when>
    <xsl:otherwise>
      <xsl:call-template name="paragraph">
        <xsl:with-param name="class" select="name(.)"/>
        <xsl:with-param name="content">
          <xsl:call-template name="person.name"/>
        </xsl:with-param>
      </xsl:call-template>
      <xsl:apply-templates mode="titlepage.mode" select="./affiliation"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- ====================================================================== -->
<!-- toc -->

<xsl:template match="slides" mode="toc">
  <xsl:variable name="id">
    <xsl:call-template name="object.id"/>
  </xsl:variable>

  <xsl:variable name="home" select="/slides"/>
  <xsl:variable name="up" select="/slides"/>
  <xsl:variable name="next" select="(foil|foilgroup)[1]"/>
  <xsl:variable name="tocfile" select="''"/>
  <xsl:variable name="dir"> <!-- MJ: added -->
    <xsl:call-template name="dbhtml-dir"/>
  </xsl:variable>

  <xsl:call-template name="write.chunk">
    <xsl:with-param name="indent" select="$output.indent"/>
    <xsl:with-param name="filename" select="concat($base.dir, $dir, $toc.html)"/>
    <xsl:with-param name="content">
      <html>
        <head>
          <title><xsl:value-of select="slidesinfo/title"/></title>

          <xsl:call-template name="system.head.content"/>

	  <meta name="generator" content="DocBook Slides Stylesheets V{$VERSION}"/>

          <!-- Links -->
          <xsl:if test="$css.stylesheet != ''">
            <link type="text/css" rel="stylesheet">
              <xsl:attribute name="href">
                <xsl:call-template name="css.stylesheet"/>
              </xsl:attribute>
            </link>
          </xsl:if>
          <xsl:apply-templates select="/processing-instruction('dbhtml')" mode="css.pi"/>

          <xsl:call-template name="links">
            <xsl:with-param name="home" select="$home"/>
            <xsl:with-param name="up" select="$up"/>
            <xsl:with-param name="next" select="$next"/>
            <xsl:with-param name="tocfile" select="$tocfile"/>
          </xsl:call-template>

          <!-- Scripts -->

          <xsl:if test="$overlay != 0 or $keyboard.nav != 0">
            <script language="javascript" type="text/javascript">
              <xsl:text> </xsl:text>
            </script>
          </xsl:if>

          <xsl:if test="$keyboard.nav != 0">
            <xsl:call-template name="ua.js"/>
            <xsl:call-template name="xbDOM.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
            <xsl:call-template name="xbLibrary.js"/>
            <script language="javascript" type="text/javascript">
              <xsl:text disable-output-escaping="yes">
                &lt;!--
                xblibrary = new xbLibrary('</xsl:text>
              <xsl:call-template name="script-dir"/>
              <xsl:text disable-output-escaping="yes">');
                // --&gt;
              </xsl:text>
            </script>
            <xsl:call-template name="xbStyle.js"/>
            <xsl:call-template name="xbCollapsibleLists.js"/>
            <xsl:call-template name="slides.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>

          <xsl:if test="$overlay != '0'">
            <xsl:call-template name="overlay.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>

          <xsl:call-template name="user.head.content"/>
        </head>

        <body class="tocpage">
          <xsl:call-template name="body.attributes"/>
          <xsl:if test="$overlay != 0">
            <xsl:attribute name="onload">
              <xsl:text>overlaySetup('lc')</xsl:text>
            </xsl:attribute>
          </xsl:if>
          <xsl:if test="$keyboard.nav != 0">
            <xsl:attribute name="onkeypress">
              <xsl:text>navigate(event)</xsl:text>
            </xsl:attribute>
          </xsl:if>

          <div id="{$id}">
            <xsl:call-template name="toc-top-nav">
              <xsl:with-param name="home" select="$home"/>
              <xsl:with-param name="up" select="$up"/>
              <xsl:with-param name="next" select="$next"/>
              <xsl:with-param name="tocfile" select="$tocfile"/>
            </xsl:call-template>

            <div class="toc-body">
              <xsl:call-template name="toc-body"/>
            </div>

            <div id="overlayDiv">
              <xsl:call-template name="overlayDiv.attributes"/>
              <xsl:call-template name="toc-bottom-nav">
                <xsl:with-param name="home" select="$home"/>
                <xsl:with-param name="up" select="$up"/>
                <xsl:with-param name="next" select="$next"/>
                <xsl:with-param name="tocfile" select="$tocfile"/>
              </xsl:call-template>
            </div>
          </div>
        </body>
      </html>
    </xsl:with-param>
  </xsl:call-template>
</xsl:template>

<xsl:template name="toc-body">
  <h1 class="title">
    <a href="{$titlefoil.html}">
      <xsl:value-of select="/slides/slidesinfo/title"/>
    </a>
  </h1>

  <p class="toctitle">
    <strong xmlns:xslo="http://www.w3.org/1999/XSL/Transform">
      <xsl:call-template name="gentext">
        <xsl:with-param name="key">TableofContents</xsl:with-param>
      </xsl:call-template>
    </strong>
  </p>
  <dl class="toc">
    <xsl:apply-templates select="foilgroup|foil" mode="toc"/>
  </dl>
</xsl:template>

<xsl:template name="toc-top-nav">
  <xsl:param name="home" select="/slides"/>
  <xsl:param name="up"/>
  <xsl:param name="prev"/>
  <xsl:param name="next" select="(foil|foilgroup)[1]"/>
  <xsl:param name="tocfile"/>

  <xsl:call-template name="top-nav">
    <xsl:with-param name="home" select="$home"/>
    <xsl:with-param name="up" select="$up"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="prev" select="$prev"/>
    <xsl:with-param name="tocfile" select="$tocfile"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="toc-bottom-nav">
  <xsl:param name="home" select="/slides"/>
  <xsl:param name="up"/>
  <xsl:param name="prev"/>
  <xsl:param name="next" select="(foil|foilgroup)[1]"/>
  <xsl:param name="tocfile"/>

  <xsl:call-template name="bottom-nav">
    <xsl:with-param name="home" select="$home"/>
    <xsl:with-param name="up" select="$up"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="prev" select="$prev"/>
    <xsl:with-param name="tocfile" select="$tocfile"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="foilgroup" mode="toc">
  <xsl:param name="recursive" select="1"/>

  <dt>
    <xsl:apply-templates select="." mode="number"/>
    <xsl:text>. </xsl:text>
    <a>
      <xsl:attribute name="href">
        <xsl:apply-templates select="." mode="filename"/>
      </xsl:attribute>
      <xsl:value-of select="title"/>
    </a>
  </dt>
  <xsl:if test="$recursive != 0">
    <dd>
      <dl class="toc">
	<xsl:apply-templates select="foil" mode="toc"/>
      </dl>
    </dd>
  </xsl:if>
</xsl:template>

<xsl:template match="foil" mode="toc">
  <dt>
    <xsl:apply-templates select="." mode="number"/>
    <xsl:text>. </xsl:text>
    <a>
      <xsl:attribute name="href">
        <xsl:apply-templates select="." mode="filename"/>
      </xsl:attribute>
      <xsl:value-of select="title"/>
    </a>
  </dt>
</xsl:template>

<xsl:template match="title|titleabbrev" mode="toc">
  <xsl:apply-templates mode="toc"/>
</xsl:template>

<xsl:template match="speakernotes" mode="toc">
  <!-- nop -->
</xsl:template>

<!-- ====================================================================== -->
<!-- foil -->

<xsl:template match="foil">
  <xsl:param name="thisfoil">
    <xsl:apply-templates select="." mode="chunk-filename"/>
  </xsl:param>

  <xsl:variable name="id">
    <xsl:call-template name="object.id"/>
  </xsl:variable>

  <xsl:variable name="home" select="/slides"/>
  <xsl:variable name="up" select="(parent::slides|parent::foilgroup)[1]"/>
  <xsl:variable name="next" select="(following::foil                                     |following::foilgroup)[1]"/>
  <xsl:variable name="prev" select="(preceding-sibling::foil[1]                                     |parent::foilgroup[1]                                     |/slides)[last()]"/>

  <xsl:call-template name="write.chunk">
    <xsl:with-param name="indent" select="$output.indent"/>
    <xsl:with-param name="filename" select="concat($base.dir, $thisfoil)"/>
    <xsl:with-param name="content">
      <html>
        <head>
          <title><xsl:value-of select="title"/></title>

          <xsl:call-template name="system.head.content"/>

	  <meta name="generator" content="DocBook Slides Stylesheets V{$VERSION}"/>

          <!-- Links -->
          <xsl:if test="$css.stylesheet != ''">
            <link type="text/css" rel="stylesheet">
              <xsl:attribute name="href">
                <xsl:call-template name="css.stylesheet"/>
              </xsl:attribute>
            </link>
          </xsl:if>
          <xsl:apply-templates select="/processing-instruction('dbhtml')" mode="css.pi"/>

          <xsl:call-template name="links">
            <xsl:with-param name="home" select="$home"/>
            <xsl:with-param name="up" select="$up"/>
            <xsl:with-param name="next" select="$next"/>
            <xsl:with-param name="prev" select="$prev"/>
          </xsl:call-template>

          <!-- Scripts -->

          <xsl:if test="$overlay != 0 or $keyboard.nav != 0">
            <script language="javascript" type="text/javascript">
              <xsl:text> </xsl:text>
            </script>
          </xsl:if>

          <xsl:if test="$keyboard.nav != 0">
            <xsl:call-template name="ua.js"/>
            <xsl:call-template name="xbDOM.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
            <xsl:call-template name="xbLibrary.js"/>
            <script language="javascript" type="text/javascript">
              <xsl:text disable-output-escaping="yes">
                &lt;!--
                xblibrary = new xbLibrary('</xsl:text>
              <xsl:call-template name="script-dir"/>
              <xsl:text disable-output-escaping="yes">');
                // --&gt;
              </xsl:text>
            </script>
            <xsl:call-template name="xbStyle.js"/>
            <xsl:call-template name="xbCollapsibleLists.js"/>
            <xsl:call-template name="slides.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>

          <xsl:if test="$overlay != '0'">
            <xsl:call-template name="overlay.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>

          <xsl:call-template name="user.head.content"/>
        </head>
        <body>
          <xsl:attribute name="class">
            <xsl:value-of select="local-name(.)"/>
            <xsl:if test="@role">
              <xsl:text>-</xsl:text>
              <xsl:value-of select="@role"/>
            </xsl:if>
          </xsl:attribute>

          <xsl:call-template name="body.attributes"/>
          <xsl:if test="$overlay != 0">
            <xsl:attribute name="onload">
              <xsl:text>overlaySetup('lc')</xsl:text>
            </xsl:attribute>
          </xsl:if>
          <xsl:if test="$keyboard.nav != 0">
            <xsl:attribute name="onkeypress">
              <xsl:text>navigate(event)</xsl:text>
            </xsl:attribute>
          </xsl:if>

          <div class="{name(.)}" id="{$id}">
            <xsl:call-template name="foil-top-nav">
              <xsl:with-param name="home" select="$home"/>
              <xsl:with-param name="up" select="$up"/>
              <xsl:with-param name="next" select="$next"/>
              <xsl:with-param name="prev" select="$prev"/>
            </xsl:call-template>

            <div class="foil-body">
              <xsl:call-template name="foil-body">
                <xsl:with-param name="home" select="$home"/>
                <xsl:with-param name="up" select="$up"/>
                <xsl:with-param name="next" select="$next"/>
                <xsl:with-param name="prev" select="$prev"/>
              </xsl:call-template>
            </div>

            <div id="overlayDiv">
              <xsl:call-template name="overlayDiv.attributes"/>
              <xsl:call-template name="foil-bottom-nav">
                <xsl:with-param name="home" select="$home"/>
                <xsl:with-param name="up" select="$up"/>
                <xsl:with-param name="next" select="$next"/>
                <xsl:with-param name="prev" select="$prev"/>
              </xsl:call-template>
            </div>
          </div>

          <xsl:call-template name="process.footnotes"/>
        </body>
      </html>
    </xsl:with-param>
  </xsl:call-template>
</xsl:template>

<xsl:template name="foil-body">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>
  <xsl:apply-templates/>
</xsl:template>

<xsl:template name="foil-top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <xsl:call-template name="top-nav">
    <xsl:with-param name="home" select="$home"/>
    <xsl:with-param name="up" select="$up"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="prev" select="$prev"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="foil-bottom-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <xsl:call-template name="bottom-nav">
    <xsl:with-param name="home" select="$home"/>
    <xsl:with-param name="up" select="$up"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="prev" select="$prev"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="foil/title">
  <h1 class="{name(.)}">
    <xsl:apply-templates/>
  </h1>
</xsl:template>

<!-- ====================================================================== -->
<!-- foilgroup -->

<xsl:template match="foilgroup">
  <xsl:param name="thisfoilgroup">
    <xsl:apply-templates select="." mode="chunk-filename"/>
  </xsl:param>

  <xsl:variable name="id">
    <xsl:call-template name="object.id"/>
  </xsl:variable>

  <xsl:variable name="home" select="/slides"/>
  <xsl:variable name="up" select="(parent::slides|parent::foilgroup)[1]"/>
  <xsl:variable name="next" select="foil[1]"/>
  <xsl:variable name="prev" select="(preceding::foil|parent::foilgroup|/slides)[last()]"/>

  <xsl:call-template name="write.chunk">
    <xsl:with-param name="indent" select="$output.indent"/>
    <xsl:with-param name="filename" select="concat($base.dir, $thisfoilgroup)"/>
    <xsl:with-param name="content">
      <html>
        <head>
          <title><xsl:value-of select="title"/></title>

          <xsl:call-template name="system.head.content"/>

	  <meta name="generator" content="DocBook Slides Stylesheets V{$VERSION}"/>

          <!-- Links -->
          <xsl:if test="$css.stylesheet != ''">
            <link type="text/css" rel="stylesheet">
              <xsl:attribute name="href">
                <xsl:call-template name="css.stylesheet"/>
              </xsl:attribute>
            </link>
          </xsl:if>
          <xsl:apply-templates select="/processing-instruction('dbhtml')" mode="css.pi"/>

          <xsl:call-template name="links">
            <xsl:with-param name="home" select="$home"/>
            <xsl:with-param name="up" select="$up"/>
            <xsl:with-param name="next" select="$next"/>
            <xsl:with-param name="prev" select="$prev"/>
          </xsl:call-template>

          <!-- Scripts -->

          <xsl:if test="$overlay != 0 or $keyboard.nav != 0">
            <script language="javascript" type="text/javascript">
              <xsl:text> </xsl:text>
            </script>
          </xsl:if>

          <xsl:if test="$keyboard.nav != 0">
            <xsl:call-template name="ua.js"/>
            <xsl:call-template name="xbDOM.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
            <xsl:call-template name="xbLibrary.js"/>
            <script language="javascript" type="text/javascript">
              <xsl:text disable-output-escaping="yes">
                &lt;!--
                xblibrary = new xbLibrary('</xsl:text>
              <xsl:call-template name="script-dir"/>
              <xsl:text disable-output-escaping="yes">');
                // --&gt;
              </xsl:text>
            </script>
            <xsl:call-template name="xbStyle.js"/>
            <xsl:call-template name="xbCollapsibleLists.js"/>
            <xsl:call-template name="slides.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>

          <xsl:if test="$overlay != '0'">
            <xsl:call-template name="overlay.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>

          <xsl:call-template name="user.head.content"/>
        </head>
        <body>
          <xsl:attribute name="class">
            <xsl:value-of select="local-name(.)"/>
            <xsl:if test="@role">
              <xsl:text>-</xsl:text>
              <xsl:value-of select="@role"/>
            </xsl:if>
          </xsl:attribute>

          <xsl:call-template name="body.attributes"/>
          <xsl:if test="$overlay != 0">
            <xsl:attribute name="onload">
              <xsl:text>overlaySetup('lc')</xsl:text>
            </xsl:attribute>
          </xsl:if>
          <xsl:if test="$keyboard.nav != 0">
            <xsl:attribute name="onkeypress">
              <xsl:text>navigate(event)</xsl:text>
            </xsl:attribute>
          </xsl:if>

          <div class="{name(.)}" id="{$id}">
            <xsl:call-template name="foilgroup-top-nav">
              <xsl:with-param name="home" select="$home"/>
              <xsl:with-param name="up" select="$up"/>
              <xsl:with-param name="next" select="$next"/>
              <xsl:with-param name="prev" select="$prev"/>
            </xsl:call-template>

	    <!-- n.b. the foilgroup-body template is responsible for generating -->
	    <!-- the foilgroup toc -->
            <div class="foilgroup-body">
              <xsl:call-template name="foilgroup-body">
                <xsl:with-param name="home" select="$home"/>
                <xsl:with-param name="up" select="$up"/>
                <xsl:with-param name="next" select="$next"/>
                <xsl:with-param name="prev" select="$prev"/>
              </xsl:call-template>
            </div>

            <div id="overlayDiv">
              <xsl:call-template name="overlayDiv.attributes"/>
              <xsl:call-template name="foilgroup-bottom-nav">
                <xsl:with-param name="home" select="$home"/>
                <xsl:with-param name="up" select="$up"/>
                <xsl:with-param name="next" select="$next"/>
                <xsl:with-param name="prev" select="$prev"/>
              </xsl:call-template>
            </div>
          </div>

          <xsl:call-template name="process.footnotes"/>
        </body>
      </html>
    </xsl:with-param>
  </xsl:call-template>

  <xsl:apply-templates select="foil"/>
</xsl:template>

<xsl:template match="foilgroup/title">
  <h1 class="{name(.)}"><xsl:apply-templates/></h1>
</xsl:template>

<xsl:template name="foilgroup-body">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <xsl:apply-templates select="*[name(.) != 'foil'                                 and name(.) != 'foilgroup']"/>

  <xsl:if test="$foilgroup.toc != 0">
    <dl class="toc">
      <xsl:apply-templates select="foil" mode="toc"/>
    </dl>
  </xsl:if>
</xsl:template>

<xsl:template name="foilgroup-top-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <xsl:call-template name="top-nav">
    <xsl:with-param name="home" select="$home"/>
    <xsl:with-param name="up" select="$up"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="prev" select="$prev"/>
  </xsl:call-template>
</xsl:template>

<xsl:template name="foilgroup-bottom-nav">
  <xsl:param name="home"/>
  <xsl:param name="up"/>
  <xsl:param name="next"/>
  <xsl:param name="prev"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <xsl:call-template name="bottom-nav">
    <xsl:with-param name="home" select="$home"/>
    <xsl:with-param name="up" select="$up"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="prev" select="$prev"/>
  </xsl:call-template>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template name="overlayDiv.attributes">
  <xsl:choose>
    <xsl:when test="$overlay != 0">
      <xsl:attribute name="style">
        <xsl:text>position: absolute; visibility: visible;</xsl:text>
      </xsl:attribute>
    </xsl:when>
    <xsl:otherwise>
      <xsl:attribute name="style">padding-top: 2in;</xsl:attribute>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template match="processing-instruction('dbhtml')" mode="css.pi">
  <xsl:variable name="href">
    <xsl:call-template name="dbhtml-attribute">
      <xsl:with-param name="pis" select="."/>
      <xsl:with-param name="attribute" select="'css-stylesheet'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:if test="$href!=''">
  <xsl:choose>
    <xsl:when test="$href = ''">
      <!-- nop -->
    </xsl:when>
    <xsl:when test="contains($href, '//')">
      <link type="text/css" rel="stylesheet" href="{$href}"/>
    </xsl:when>
    <xsl:when test="starts-with($href, '/')">
      <link type="text/css" rel="stylesheet" href="{$href}"/>
    </xsl:when>
    <xsl:otherwise>
      <link type="text/css" rel="stylesheet">
        <xsl:attribute name="href">
          <xsl:call-template name="css-file">
            <xsl:with-param name="css" select="$href"/>
          </xsl:call-template>
        </xsl:attribute>
      </link>
    </xsl:otherwise>
  </xsl:choose>
  </xsl:if>

</xsl:template>

<!-- ====================================================================== -->

<xsl:template match="foil" mode="number">
  <xsl:number count="foil|foilgroup" level="any"/>
</xsl:template>

<xsl:template match="foilgroup" mode="number">
  <xsl:number count="foil|foilgroup" level="any"/>
</xsl:template>

<!-- ====================================================================== -->

<xsl:template match="slides" mode="filename">
  <xsl:value-of select="$titlefoil.html"/>
</xsl:template>

<xsl:template match="foil" mode="filename">
  <xsl:text>foil</xsl:text>
  <xsl:number count="foil" level="any" format="01"/>
  <xsl:value-of select="$html.ext"/>
</xsl:template>

<xsl:template match="foilgroup" mode="filename">
  <xsl:text>foilgroup</xsl:text>
  <xsl:number count="foilgroup" level="any" format="01"/>
  <xsl:value-of select="$html.ext"/>
</xsl:template>

<!-- ============================================================ -->

<xsl:template match="processing-instruction('Pub')">
  <xsl:variable name="pidata"><xsl:value-of select="(.)"/></xsl:variable>
  <xsl:choose>
    <xsl:when test="contains($pidata,'UDT')"/>
    <xsl:when test="contains($pidata,'/_font')">
      <xsl:text disable-output-escaping="yes">&lt;/span&gt;</xsl:text>
    </xsl:when>
    <xsl:when test="contains($pidata,'_font')">
      <xsl:text disable-output-escaping="yes">&lt;span </xsl:text>
      <xsl:choose>
        <xsl:when test="contains($pidata,'green')">class="green"</xsl:when>
        <xsl:when test="contains($pidata,'blue')">class="blue"</xsl:when>
        <xsl:when test="contains($pidata,'orange')">class="orange"</xsl:when>
        <xsl:when test="contains($pidata,'red')">class="red"</xsl:when>
        <xsl:when test="contains($pidata,'brown')">class="brown"</xsl:when>
        <xsl:when test="contains($pidata,'violet')">class="violet"</xsl:when>
        <xsl:when test="contains($pidata,'black')">class="black"</xsl:when>
        <xsl:otherwise>class="bold"</xsl:otherwise>
      </xsl:choose>
      <xsl:text disable-output-escaping="yes">&gt;</xsl:text>
    </xsl:when>
  </xsl:choose>
</xsl:template>

<!-- ============================================================ -->
<!-- blocks -->

<xsl:template match="figure">
  <div class="{name(.)}">
    <xsl:apply-imports/>
  </div>
  <xsl:if test="following-sibling::*"><hr/></xsl:if>
</xsl:template>

<xsl:template match="copyright" mode="slide.footer.mode">
  <span class="{name(.)}">
    <xsl:call-template name="gentext">
      <xsl:with-param name="key" select="'Copyright'"/>
    </xsl:call-template>
    <xsl:call-template name="gentext.space"/>
    <xsl:call-template name="dingbat">
      <xsl:with-param name="dingbat">copyright</xsl:with-param>
    </xsl:call-template>
    <xsl:call-template name="gentext.space"/>
    <xsl:call-template name="copyright.years">
      <xsl:with-param name="years" select="year"/>
      <xsl:with-param name="print.ranges" select="$make.year.ranges"/>
      <xsl:with-param name="single.year.ranges" select="$make.single.year.ranges"/>
    </xsl:call-template>
    <xsl:call-template name="gentext.space"/>
    <xsl:apply-templates select="holder" mode="titlepage.mode"/>
  </span>
</xsl:template>

<!-- ============================================================ -->
<!-- inlines -->

<xsl:template match="link">
  <xsl:call-template name="link">
    <xsl:with-param name="a.target" select="'foil'"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="ulink">
  <a>
    <xsl:if test="@id">
      <xsl:attribute name="id"><xsl:value-of select="@id"/></xsl:attribute>
    </xsl:if>
    <xsl:attribute name="href"><xsl:value-of select="@url"/></xsl:attribute>
    <xsl:if test="$ulink.target != ''">
      <xsl:attribute name="target">
        <xsl:value-of select="$ulink.target"/>
      </xsl:attribute>
    </xsl:if>
    <xsl:choose>
      <xsl:when test="count(child::node())=0">
	<xsl:value-of select="@url"/>
      </xsl:when>
      <xsl:otherwise>
	<xsl:apply-templates/>
        <xsl:if test="@role='show'">
          <xsl:text> (</xsl:text>
          <xsl:value-of select="@url"/>
          <xsl:text>)</xsl:text>
        </xsl:if>
      </xsl:otherwise>
    </xsl:choose>
  </a>
</xsl:template>

<xsl:template match="title/ulink">
  <a>
    <xsl:if test="@id">
      <xsl:attribute name="id"><xsl:value-of select="@id"/></xsl:attribute>
    </xsl:if>
    <xsl:attribute name="href"><xsl:value-of select="@url"/></xsl:attribute>
    <xsl:if test="$ulink.target != ''">
      <xsl:attribute name="target">
        <xsl:value-of select="$ulink.target"/>
      </xsl:attribute>
    </xsl:if>
    <xsl:choose>
      <xsl:when test="count(child::node())=0">
	<xsl:value-of select="@url"/>
      </xsl:when>
      <xsl:otherwise>
	<xsl:apply-templates/>
      </xsl:otherwise>
    </xsl:choose>
  </a>
</xsl:template>

<xsl:template match="subtitle">
  <h2 class="subtitle">
    <xsl:apply-templates/>
  </h2>
</xsl:template>

<xsl:template match="graphic">
  <center>
    <!-- can't this be done a better way? -->
    <xsl:apply-imports/>
  </center>
</xsl:template>

<xsl:template match="titleabbrev">
  <!-- nop -->
</xsl:template>

<xsl:template match="speakernotes">
  <!-- nop -->
</xsl:template>

<!-- ====================================================================== -->
<!-- Chunking for slides -->

<xsl:template name="chunk">
  <xsl:param name="node" select="."/>
  <xsl:choose>
    <xsl:when test="name($node)='slides'">1</xsl:when>
    <xsl:when test="name($node)='foilgroup'">1</xsl:when>
    <xsl:when test="name($node)='foil'">1</xsl:when>
    <xsl:otherwise>0</xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template match="*" mode="chunk-filename">
  <xsl:param name="recursive">0</xsl:param>
  <!-- returns the filename of a chunk -->
  <xsl:variable name="ischunk"><xsl:call-template name="chunk"/></xsl:variable>
  <xsl:variable name="filename">
    <xsl:call-template name="pi.dbhtml_filename"/>
  </xsl:variable>
  <xsl:variable name="dir">
    <xsl:call-template name="dbhtml-dir"/>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="$ischunk='0'">
      <!-- if called on something that isn't a chunk, walk up... -->
      <xsl:choose>
        <xsl:when test="count(./parent::*)&gt;0">
          <xsl:apply-templates mode="chunk-filename" select="./parent::*">
            <xsl:with-param name="recursive" select="$recursive"/>
          </xsl:apply-templates>
        </xsl:when>
        <!-- unless there is no up, in which case return "" -->
        <xsl:otherwise/>
      </xsl:choose>
    </xsl:when>

    <xsl:when test="not($recursive) and $filename != ''">
      <!-- if this chunk has an explicit name, use it -->
      <xsl:if test="$dir != ''">
        <xsl:value-of select="$dir"/>
        <xsl:text>/</xsl:text>
      </xsl:if>
      <xsl:value-of select="$filename"/>
    </xsl:when>

    <xsl:when test="name(.)='foil'">
      <xsl:variable name="foilnumber">
	<xsl:number count="foil" level="any"/>
      </xsl:variable>

      <xsl:value-of select="$dir"/>
      <xsl:text>foil</xsl:text>
      <xsl:number value="$foilnumber" format="01"/>
      <xsl:value-of select="$html.ext"/>
    </xsl:when>

    <xsl:when test="name(.)='foilgroup'">
      <xsl:variable name="foilgroupnumber">
        <xsl:number count="foilgroup" level="any" format="01"/>
      </xsl:variable>

      <xsl:value-of select="$dir"/>
      <xsl:text>foilgroup</xsl:text>
      <xsl:number value="$foilgroupnumber" format="01"/>
      <xsl:value-of select="$html.ext"/>
    </xsl:when>

    <xsl:otherwise>
      <xsl:text>chunk-filename-error-</xsl:text>
      <xsl:value-of select="name(.)"/>
      <xsl:number level="any" format="01" from="set"/>
      <xsl:if test="not($recursive)">
        <xsl:value-of select="$html.ext"/>
      </xsl:if>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- ====================================================================== -->
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

<xsl:template name="foil.number">
  <xsl:choose>
    <xsl:when test="$show.foil.number != 0 and self::foil">
      <xsl:number count="foil" level="any"/>
      /
      <xsl:value-of select="count(//foil)"/>
    </xsl:when>
    <xsl:otherwise>
      &#160;
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

</xsl:stylesheet>
