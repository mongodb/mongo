<?xml version="1.0" encoding="ASCII"?>
<!--This file was created automatically by html2xhtml-->
<!--from the HTML stylesheets.-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns="http://www.w3.org/1999/xhtml" version="1.0">

<xsl:import href="slides-common.xsl"/>

<xsl:param name="but-fforward.png" select="'active/but-fforward.png'"/>
<xsl:param name="but-info.png" select="'active/but-info.png'"/>
<xsl:param name="but-next.png" select="'active/but-next.png'"/>
<xsl:param name="but-prev.png" select="'active/but-prev.png'"/>
<xsl:param name="but-rewind.png" select="'active/but-rewind.png'"/>

<xsl:param name="but-xfforward.png" select="'inactive/but-fforward.png'"/>
<xsl:param name="but-xinfo.png" select="'inactive/but-info.png'"/>
<xsl:param name="but-xnext.png" select="'inactive/but-next.png'"/>
<xsl:param name="but-xprev.png" select="'inactive/but-prev.png'"/>
<xsl:param name="but-xrewind.png" select="'inactive/but-rewind.png'"/>

<!-- overrides for this stylesheet -->
<xsl:param name="titlefoil.html" select="concat('index', $html.ext)"/>
<xsl:param name="toc.width" select="40"/>

<!-- ============================================================ -->

<xsl:template match="slides">
  <xsl:call-template name="write.chunk">
    <xsl:with-param name="indent" select="$output.indent"/>
    <xsl:with-param name="filename" select="concat($base.dir, $toc.html)"/>
    <xsl:with-param name="content">
      <html>
        <head>
          <title><xsl:value-of select="slidesinfo/title"/></title>
          <xsl:if test="$css.stylesheet != ''">
            <link type="text/css" rel="stylesheet">
              <xsl:attribute name="href">
                <xsl:call-template name="css.stylesheet"/>
              </xsl:attribute>
            </link>
          </xsl:if>
          <xsl:apply-templates select="/processing-instruction('dbhtml')" mode="css.pi"/>

          <xsl:call-template name="links">
            <xsl:with-param name="next" select="/slides"/>
            <xsl:with-param name="tocfile" select="$toc.html"/>
          </xsl:call-template>

          <xsl:if test="$keyboard.nav != 0">
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
                xblibrary = new xbLibrary('../browser');
                // --&gt;
              </xsl:text>
            </script>
            <xsl:call-template name="xbStyle.js"/>
            <xsl:call-template name="xbCollapsibleLists.js"/>
            <xsl:call-template name="slides.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>
        </head>
        <body class="tocpage">
          <xsl:call-template name="body.attributes"/>
          <xsl:if test="$keyboard.nav != 0">
            <xsl:attribute name="onkeypress">
              <xsl:text>navigate(event)</xsl:text>
            </xsl:attribute>
          </xsl:if>

          <table border="0" width="100%" summary="Navigation and body table" cellpadding="0" cellspacing="0">
            <tr>
              <td>&#160;</td>
              <td><xsl:apply-templates select="." mode="header"/></td>
            </tr>

            <tr>
              <td width="{$toc.width}" valign="top" align="left">
		<xsl:if test="$toc.bg.color != ''">
		  <xsl:attribute name="bgcolor">
		    <xsl:value-of select="$toc.bg.color"/>
		  </xsl:attribute>
		</xsl:if>

                <xsl:call-template name="vertical-navigation">
                  <xsl:with-param name="next" select="/slides"/>
                  <xsl:with-param name="tocfile"/>
                </xsl:call-template>

              </td>
              <td valign="top" align="left">
		<xsl:if test="$body.bg.color != ''">
		  <xsl:attribute name="bgcolor">
		    <xsl:value-of select="$body.bg.color"/>
		  </xsl:attribute>
		</xsl:if>

                <div class="{name(.)}">

                  <div class="toc-body">
                    <xsl:call-template name="toc-body"/>
                  </div>

                </div>
              </td>
            </tr>

            <tr>
              <td>&#160;</td>
              <td><xsl:apply-templates select="." mode="footer"/></td>
            </tr>
          </table>
        </body>
      </html>
    </xsl:with-param>
  </xsl:call-template>

  <xsl:apply-templates/>
</xsl:template>

<xsl:template match="slidesinfo">
  <xsl:call-template name="write.chunk">
    <xsl:with-param name="indent" select="$output.indent"/>
    <xsl:with-param name="filename" select="concat($base.dir, $titlefoil.html)"/>
    <xsl:with-param name="content">
      <html>
        <head>
          <title><xsl:value-of select="title"/></title>
          <xsl:if test="$css.stylesheet != ''">
            <link type="text/css" rel="stylesheet">
              <xsl:attribute name="href">
                <xsl:call-template name="css.stylesheet"/>
              </xsl:attribute>
            </link>
          </xsl:if>
          <xsl:apply-templates select="/processing-instruction('dbhtml')" mode="css.pi"/>

          <xsl:call-template name="links">
            <xsl:with-param name="next" select="(/slides/foil|/slides/foilgroup)[1]"/>
            <xsl:with-param name="tocfile" select="$toc.html"/>
          </xsl:call-template>

          <xsl:if test="$keyboard.nav != 0">
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
                xblibrary = new xbLibrary('../browser');
                // --&gt;
              </xsl:text>
            </script>
            <xsl:call-template name="xbStyle.js"/>
            <xsl:call-template name="xbCollapsibleLists.js"/>
            <xsl:call-template name="slides.js">
              <xsl:with-param name="language" select="'javascript'"/>
            </xsl:call-template>
          </xsl:if>
        </head>
        <body class="titlepage">
          <xsl:call-template name="body.attributes"/>
          <xsl:if test="$keyboard.nav != 0">
            <xsl:attribute name="onkeypress">
              <xsl:text>navigate(event)</xsl:text>
            </xsl:attribute>
          </xsl:if>

          <table border="0" width="100%" summary="Navigation and body table" cellpadding="0" cellspacing="0">
            <tr>
              <td>&#160;</td>
              <td><xsl:apply-templates select="." mode="header"/></td>
            </tr>

            <tr>
              <td width="{$toc.width}" valign="top" align="left">
		<xsl:if test="$toc.bg.color != ''">
		  <xsl:attribute name="bgcolor">
		    <xsl:value-of select="$toc.bg.color"/>
		  </xsl:attribute>
		</xsl:if>

                <xsl:call-template name="vertical-navigation">
                  <xsl:with-param name="first"/>
                  <xsl:with-param name="last" select="(following::foilgroup|following::foil)[last()]"/>
                  <xsl:with-param name="next" select="(following::foilgroup|following::foil)[1]"/>
                </xsl:call-template>

              </td>
              <td valign="top" align="left">
		<xsl:if test="$body.bg.color != ''">
		  <xsl:attribute name="bgcolor">
		    <xsl:value-of select="$body.bg.color"/>
		  </xsl:attribute>
		</xsl:if>
                <div class="{name(.)}">
                  <xsl:apply-templates mode="titlepage.mode"/>
                </div>
              </td>
            </tr>

            <tr>
              <td>&#160;</td>
              <td><xsl:apply-templates select="." mode="footer"/></td>
            </tr>
          </table>
        </body>
      </html>
    </xsl:with-param>
  </xsl:call-template>
</xsl:template>

<xsl:template match="foilgroup">
  <xsl:param name="thisfoilgroup">
    <xsl:apply-templates select="." mode="filename"/>
  </xsl:param>

  <xsl:variable name="id">
    <xsl:call-template name="object.id"/>
  </xsl:variable>

  <xsl:variable name="nextfoil" select="foil[1]"/>
  <xsl:variable name="lastfoil" select="(descendant::foil|following::foil)[last()]"/>
  <xsl:variable name="prevfoil" select="(preceding::foil|/slides)[last()]"/>

  <xsl:call-template name="write.chunk">
    <xsl:with-param name="indent" select="$output.indent"/>
    <xsl:with-param name="filename" select="concat($base.dir, $thisfoilgroup)"/>
    <xsl:with-param name="content">
      <html>
	<head>
	  <title><xsl:value-of select="title"/></title>
	  <xsl:if test="$css.stylesheet != ''">
	    <link type="text/css" rel="stylesheet">
	      <xsl:attribute name="href">
		<xsl:call-template name="css.stylesheet"/>
	      </xsl:attribute>
	    </link>
	  </xsl:if>
	  <xsl:apply-templates select="/processing-instruction('dbhtml')" mode="css.pi"/>

	  <xsl:call-template name="links">
	    <xsl:with-param name="prev" select="$prevfoil"/>
	    <xsl:with-param name="next" select="$nextfoil"/>
	  </xsl:call-template>
	  
	  <xsl:if test="$keyboard.nav != 0">
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
                xblibrary = new xbLibrary('../browser');
                // --&gt;
              </xsl:text>
            </script>
	    <xsl:call-template name="xbStyle.js"/>
	    <xsl:call-template name="xbCollapsibleLists.js"/>
	    <xsl:call-template name="slides.js">
	      <xsl:with-param name="language" select="'javascript'"/>
	    </xsl:call-template>
	  </xsl:if>
	</head>
	<body class="foilgroup">
	  <xsl:call-template name="body.attributes"/>
	  <xsl:if test="$keyboard.nav != 0">
	    <xsl:attribute name="onkeypress">
	      <xsl:text>navigate(event)</xsl:text>
	    </xsl:attribute>
	  </xsl:if>

	  <table border="0" width="100%" summary="Navigation and body table" cellpadding="0" cellspacing="0">
	    <tr>
	      <td>&#160;</td>
	      <td><xsl:apply-templates select="." mode="header"/></td>
	    </tr>
	    
	    <tr>
	      <td width="{$toc.width}" valign="top" align="left">
		<xsl:if test="$toc.bg.color != ''">
		  <xsl:attribute name="bgcolor">
		    <xsl:value-of select="$toc.bg.color"/>
		  </xsl:attribute>
		</xsl:if>
		
		<xsl:call-template name="vertical-navigation">
		  <xsl:with-param name="last" select="$lastfoil"/>
		  <xsl:with-param name="prev" select="$prevfoil"/>
		  <xsl:with-param name="next" select="$nextfoil"/>
		</xsl:call-template>
		
	      </td>
	      <td valign="top" align="left">
		<xsl:if test="$body.bg.color != ''">
		  <xsl:attribute name="bgcolor">
		    <xsl:value-of select="$body.bg.color"/>
		  </xsl:attribute>
		</xsl:if>

		<div class="{name(.)}">
		  <xsl:apply-templates/>
		</div>
	      </td>
	    </tr>

	    <tr>
	      <td>&#160;</td>
	      <td><xsl:apply-templates select="." mode="footer"/></td>
	    </tr>
	  </table>
	</body>
      </html>
    </xsl:with-param>
  </xsl:call-template>

  <xsl:apply-templates select="foil"/>
</xsl:template>

<xsl:template match="foil">
  <xsl:variable name="id">
    <xsl:call-template name="object.id"/>
  </xsl:variable>

  <xsl:variable name="foilgroup" select="ancestor::foilgroup"/>

  <xsl:variable name="thisfoil">
    <xsl:apply-templates select="." mode="filename"/>
  </xsl:variable>

  <xsl:variable name="nextfoil" select="(following::foil                                         |following::foilgroup)[1]"/>

  <xsl:variable name="lastfoil" select="following::foil[last()]"/>

  <xsl:variable name="prevfoil" select="(preceding-sibling::foil[1]                                         |parent::foilgroup[1]                                         |/slides)[last()]"/>

  <xsl:call-template name="write.chunk">
    <xsl:with-param name="indent" select="$output.indent"/>
    <xsl:with-param name="filename" select="concat($base.dir, $thisfoil)"/>
    <xsl:with-param name="content">
      <html>
	<head>
	  <title><xsl:value-of select="title"/></title>
	  <xsl:if test="$css.stylesheet != ''">
	    <link type="text/css" rel="stylesheet">
	      <xsl:attribute name="href">
		<xsl:call-template name="css.stylesheet"/>
	      </xsl:attribute>
	    </link>
	  </xsl:if>
	  <xsl:apply-templates select="/processing-instruction('dbhtml')" mode="css.pi"/>

	  <xsl:call-template name="links">
	    <xsl:with-param name="prev" select="$prevfoil"/>
	    <xsl:with-param name="next" select="$nextfoil"/>
	  </xsl:call-template>

	  <xsl:if test="$keyboard.nav != 0">
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
                xblibrary = new xbLibrary('../browser');
                // --&gt;
              </xsl:text>
            </script>
	    <xsl:call-template name="xbStyle.js"/>
	    <xsl:call-template name="xbCollapsibleLists.js"/>
	    <xsl:call-template name="slides.js">
	      <xsl:with-param name="language" select="'javascript'"/>
	    </xsl:call-template>
	  </xsl:if>
	</head>
	<body class="foil">
	  <xsl:call-template name="body.attributes"/>
	  <xsl:if test="$keyboard.nav != 0">
	    <xsl:attribute name="onkeypress">
	      <xsl:text>navigate(event)</xsl:text>
	    </xsl:attribute>
	  </xsl:if>

	  <table border="0" width="100%" summary="Navigation and body table" cellpadding="0" cellspacing="0">
	    <tr>
	      <td>&#160;</td>
	      <td><xsl:apply-templates select="." mode="header"/></td>
	    </tr>

	    <tr>
	      <td width="{$toc.width}" valign="top" align="left">
		<xsl:if test="$toc.bg.color != ''">
		  <xsl:attribute name="bgcolor">
		    <xsl:value-of select="$toc.bg.color"/>
		  </xsl:attribute>
		</xsl:if>

		<xsl:call-template name="vertical-navigation">
		  <xsl:with-param name="last" select="$lastfoil"/>
		  <xsl:with-param name="prev" select="$prevfoil"/>
		  <xsl:with-param name="next" select="$nextfoil"/>
		</xsl:call-template>

	      </td>
	      <td valign="top" align="left">
		<xsl:if test="$body.bg.color != ''">
		  <xsl:attribute name="bgcolor">
		    <xsl:value-of select="$body.bg.color"/>
		  </xsl:attribute>
		</xsl:if>

		<div class="{name(.)}">
		  <xsl:apply-templates/>
		</div>
	      </td>
	    </tr>

	    <tr>
	      <td>&#160;</td>
	      <td><xsl:apply-templates select="." mode="footer"/></td>
	    </tr>
	  </table>
	</body>
      </html>
    </xsl:with-param>
  </xsl:call-template>
</xsl:template>

<!-- ============================================================ -->

<xsl:template match="slidesinfo" mode="header">
  <div class="navhead">
    <!-- nop -->
  </div>
</xsl:template>

<xsl:template match="foil|foilgroup" mode="header">
  <div class="navhead">
    <table border="0" width="100%" summary="Header table" cellpadding="0" cellspacing="0">
      <tr>
        <td align="left">
          <xsl:apply-templates select="/slides/slidesinfo/title" mode="slide.footer.mode"/>
        </td>
        <td align="right">
          <xsl:value-of select="count(preceding::foil)                                 + count(preceding::foilgroup)                                 + count(ancestor::foilgroup)                                 + 1"/>
        </td>
      </tr>
    </table>
  </div>
</xsl:template>

<xsl:template match="slidesinfo" mode="footer">
  <div class="navfoot">
    <!-- nop -->
  </div>
</xsl:template>

<xsl:template match="foil|foilgroup" mode="footer">
  <div class="navfoot">
    <table border="0" width="100%" summary="Header table" cellpadding="0" cellspacing="0">
      <tr>
        <td align="center">
          <xsl:text>Slide </xsl:text>
          <xsl:value-of select="count(preceding::foil)                                 + count(preceding::foilgroup)                                 + count(ancestor::foilgroup)                                 + 1"/>
          <xsl:text> of </xsl:text>
          <xsl:value-of select="count(//foil) + count(//foilgroup)"/>
        </td>
      </tr>
    </table>
  </div>
</xsl:template>

<xsl:template match="slides" mode="footer"/>

<!-- ============================================================ -->

<xsl:template name="vertical-navigation">
  <xsl:param name="first" select="/slides"/>
  <xsl:param name="prev"/>
  <xsl:param name="last"/>
  <xsl:param name="next"/>
  <xsl:param name="tocfile" select="$toc.html"/>

  <div class="vnav">
    <xsl:choose>
      <xsl:when test="$first">
        <a>
          <xsl:attribute name="href">
            <xsl:apply-templates select="$first" mode="filename"/>
          </xsl:attribute>
          <img border="0" alt="First">
            <xsl:attribute name="src">
              <xsl:call-template name="graphics-file">
                <xsl:with-param name="image" select="$but-rewind.png"/>
              </xsl:call-template>
            </xsl:attribute>
          </img>
        </a>
      </xsl:when>
      <xsl:otherwise>
        <img alt="First">
          <xsl:attribute name="src">
            <xsl:call-template name="graphics-file">
              <xsl:with-param name="image" select="$but-xrewind.png"/>
            </xsl:call-template>
          </xsl:attribute>
        </img>
      </xsl:otherwise>
    </xsl:choose>
    <br/>
    <xsl:choose>
      <xsl:when test="$prev">
        <a>
          <xsl:attribute name="href">
            <xsl:apply-templates select="$prev" mode="filename"/>
          </xsl:attribute>
          <img border="0" alt="Previous">
            <xsl:attribute name="src">
              <xsl:call-template name="graphics-file">
                <xsl:with-param name="image" select="$but-prev.png"/>
              </xsl:call-template>
            </xsl:attribute>
          </img>
        </a>
      </xsl:when>
      <xsl:otherwise>
        <img alt="Previous">
          <xsl:attribute name="src">
            <xsl:call-template name="graphics-file">
              <xsl:with-param name="image" select="$but-xprev.png"/>
            </xsl:call-template>
          </xsl:attribute>
        </img>
      </xsl:otherwise>
    </xsl:choose>
    <br/>
    <xsl:choose>
      <xsl:when test="$next">
        <a>
          <xsl:attribute name="href">
            <xsl:apply-templates select="$next" mode="filename"/>
          </xsl:attribute>
          <img border="0" alt="Last">
            <xsl:attribute name="src">
              <xsl:call-template name="graphics-file">
                <xsl:with-param name="image" select="$but-next.png"/>
              </xsl:call-template>
            </xsl:attribute>
          </img>
        </a>
      </xsl:when>
      <xsl:otherwise>
        <img alt="Last">
          <xsl:attribute name="src">
            <xsl:call-template name="graphics-file">
              <xsl:with-param name="image" select="$but-xnext.png"/>
            </xsl:call-template>
          </xsl:attribute>
        </img>
      </xsl:otherwise>
    </xsl:choose>
    <br/>
    <xsl:choose>
      <xsl:when test="$last">
        <a>
          <xsl:attribute name="href">
            <xsl:apply-templates select="$last" mode="filename"/>
          </xsl:attribute>
          <img border="0" alt="Next">
            <xsl:attribute name="src">
              <xsl:call-template name="graphics-file">
                <xsl:with-param name="image" select="$but-fforward.png"/>
              </xsl:call-template>
            </xsl:attribute>
          </img>
        </a>
      </xsl:when>
      <xsl:otherwise>
        <img alt="Next">
          <xsl:attribute name="src">
            <xsl:call-template name="graphics-file">
              <xsl:with-param name="image" select="$but-xfforward.png"/>
            </xsl:call-template>
          </xsl:attribute>
        </img>
      </xsl:otherwise>
    </xsl:choose>

    <br/>
    <br/>

    <xsl:choose>
      <xsl:when test="$tocfile != ''">
        <a href="{$tocfile}">
          <img border="0" alt="ToC">
            <xsl:attribute name="src">
              <xsl:call-template name="graphics-file">
                <xsl:with-param name="image" select="$but-info.png"/>
              </xsl:call-template>
            </xsl:attribute>
          </img>
        </a>
      </xsl:when>
      <xsl:otherwise>
        <img border="0" alt="ToC">
          <xsl:attribute name="src">
            <xsl:call-template name="graphics-file">
              <xsl:with-param name="image" select="$but-xinfo.png"/>
            </xsl:call-template>
          </xsl:attribute>
        </img>
      </xsl:otherwise>
    </xsl:choose>
  </div>
</xsl:template>

</xsl:stylesheet>
