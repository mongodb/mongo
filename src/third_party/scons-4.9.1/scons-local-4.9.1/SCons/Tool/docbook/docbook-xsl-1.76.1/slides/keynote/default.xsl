<xsl:stylesheet version='1.0'
  xmlns:xsl='http://www.w3.org/1999/XSL/Transform'
  xmlns='http://developer.apple.com/schemas/APXL'
  xmlns:apxl='http://developer.apple.com/schemas/APXL'
  xmlns:plugin='http://developer.apple.com/schemas/APXLPlugins'
  xmlns:doc='http://nwalsh.com/xsl/documentation/1.0'
  xmlns:str='http://xsltsl.org/string'
  xmlns:math='http://xsltsl.org/math'
  exclude-result-prefixes='doc str math'>

  <xsl:import href='xsltsl/stdlib.xsl'/>
  <xsl:output method='xml' indent='yes' encoding='UTF-8'/>
  <xsl:strip-space elements='*'/>

  <doc:article xmlns=''>
    <articleinfo>
      <title>Keynote Slides</title>

      <author>
        <firstname>Steve</firstname>
        <surname>Ball</surname>
        <affiliation>
          <orgname>Zveno</orgname>
          <address>
            <otheraddr>
              <ulink url='http://www.zveno.com/'>zveno.com</ulink>
            </otheraddr>
          </address>
        </affiliation>
      </author>

      <releaseinfo>$Id: default.xsl 3991 2004-11-10 06:51:55Z balls $</releaseinfo>

      <copyright>
        <year>2004</year>
        <year>2003</year>
        <holder>Steve Ball, Zveno Pty Ltd</holder>
      </copyright>

      <legalnotice>
        <para>Zveno Pty Ltd makes this software and associated documentation available free of charge for any purpose.  You may make copies of the software but you must include all of this notice on any copy.</para>
        <para>Zveno Pty Ltd does not warrant that this software is error free or fit for any purpose.  Zveno Pty Ltd disclaims any liability for all claims, expenses, losses, damages and costs any user may incur as a result of using, copying or modifying the software.</para>
      </legalnotice>
    </articleinfo>
  </doc:article>

  <xsl:param name='slides'/>

  <xsl:variable name='slide-master' select='"Title &amp; Subtitle"'/>
  <xsl:variable name='foilgroup-master' select='"Title - Center"'/>
  <xsl:variable name='overview-master' select='"Title - Top"'/>
  <xsl:variable name='bullet-master' select='"SmlTitle &amp; Bullets"'/>
  <xsl:variable name='bullet-and-image-master' select='"Title &amp; Bullets - Left"'/>
  <xsl:variable name='title-only-master' select='"SmlTitle"'/>

  <xsl:variable name='masters' select='/apxl:presentation/apxl:theme/apxl:master-slides'/>

  <xsl:template match='/'>
    <xsl:choose>
      <xsl:when test='$slides = ""'>
        <xsl:message terminate='yes'>You must specify your slides document using the "slides" parameter</xsl:message>
      </xsl:when>
      <xsl:otherwise>
        <xsl:apply-templates/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match='apxl:slide-list'>
    <slide-list>
      <xsl:apply-templates select='document($slides)' mode='slides'/>
    </slide-list>
  </xsl:template>

  <xsl:template match='slides' mode='slides'>
    <slide id='slide-1' master-slide-id="{$masters/apxl:master-slide[@name=$slide-master]/@id}">
      <drawables>
        <body visibility='tracks-master' vertical-alignment='tracks-master'/>
        <title visibility='tracks-master' vertical-alignment='tracks-master'/>
      </drawables>
      <transition-style type='inherited'/>
      <thumbnails>
        <thumbnail file='thumbs/st0.tiff' byte-size='6520' size='60 45'/>
      </thumbnails>
      <bullets>
        <bullet marker-type='inherited' level='0'>
          <content tab-stops='L 96' font-size='84' font-color='g1' font-name='GillSans' paragraph-alignment='center'>
            <xsl:apply-templates select='slidesinfo/title/node()'/>
          </content>
        </bullet>
        <xsl:choose>
          <xsl:when test='slidesinfo/subtitle'>
            <bullet marker-type='inherited' level='1'>
              <content tab-stops='L 96' font-size='36' font-color='g1' font-name='GillSans' paragraph-alignment='center'>
                <xsl:apply-templates select='slidesinfo/subtitle/node()' mode='slides'/>
              </content>
            </bullet>
          </xsl:when>
          <xsl:when test='slidesinfo/corpauthor'>
            <bullet marker-type='inherited' level='1'>
              <content tab-stops='L 96' font-size='36' font-color='g1' font-name='GillSans' paragraph-alignment='center'>
                <xsl:apply-templates select='slidesinfo/corpauthor/node()' mode='slides'/>
              </content>
            </bullet>
          </xsl:when>
          <xsl:when test='slidesinfo/author'>
            <bullet marker-type='inherited' level='1'>
              <content tab-stops='L 96' font-size='36' font-color='g1' font-name='GillSans' paragraph-alignment='center'>
                <xsl:apply-templates select='slidesinfo/author' mode='slides'/>
              </content>
            </bullet>
          </xsl:when>
        </xsl:choose>
      </bullets>
      <notes font-size='18' font-name='LucidaGrande'>
        <xsl:apply-templates select='slidesinfo/*[not(self::title|self::subtitle|self::corpauthor|self::author)]' mode='slides'/>
      </notes>
    </slide>

    <xsl:if test='foilgroup'>
      <xsl:call-template name='overview'/>
    </xsl:if>

    <xsl:apply-templates select='foilgroup|foil' mode='slides'/>
  </xsl:template>

  <xsl:template name='overview'>
    <xsl:param name='current' select='/'/>

    <slide id='overview-{generate-id()}' master-slide-id="{$masters/apxl:master-slide[@name=$overview-master]/@id}">
      <drawables>
        <body visibility='tracks-master' vertical-alignment='tracks-master'/>
        <title visibility='tracks-master' vertical-alignment='tracks-master'/>

        <xsl:for-each select='ancestor-or-self::slides/foilgroup'>
          <textbox id='textbox-{position()}' grow-horizontally='true' transformation='1 0 0 1 {100 + floor((position() - 1) div 10) * 400} {200 + floor((position() - 1) mod 10) * 50}' size='200 50'>
            <content tab-stops='L 84' font-size='36' paragraph-alignment='left'>
              <xsl:attribute name='font-color'>
                <xsl:choose>
                  <xsl:when test='generate-id() = generate-id($current)'>
                    <xsl:text>1 0.5 0</xsl:text>
                  </xsl:when>
                  <xsl:otherwise>g1</xsl:otherwise>
                </xsl:choose>
              </xsl:attribute>
              <xsl:apply-templates select='title' mode='slides'/>
            </content>
          </textbox>
        </xsl:for-each>

      </drawables>
      <transition-style type='inherited'/>
      <thumbnails>
        <thumbnail file='thumbs/st0.tiff' byte-size='6520' size='60 45'/>
      </thumbnails>
      <bullets>
        <bullet marker-type='inherited' level='0'>
          <content tab-stops='L 96' font-size='84' font-color='g1' font-name='GillSans' paragraph-alignment='center'>Overview</content>
        </bullet>
      </bullets>
    </slide>
  </xsl:template>

  <xsl:template match='author' mode='slides'>
    <xsl:apply-templates select='firstname/node()' mode='slides'/>
    <xsl:text> </xsl:text>
    <xsl:apply-templates select='surname/node()' mode='slides'/>
  </xsl:template>
  <xsl:template match='copyright' mode='slides'>
    <xsl:text>Copyright (c) </xsl:text>
    <xsl:value-of select='year'/>
    <xsl:text> </xsl:text>
    <xsl:apply-templates select='holder' mode='slides'/>
    <xsl:text>.  </xsl:text>
  </xsl:template>

  <xsl:template match='foilgroup' mode='slides'>
    <xsl:variable name='number' select='count(preceding-sibling::foilgroup) + count(preceding::foil) + 1'/>

    <xsl:call-template name='overview'>
      <xsl:with-param name='current' select='.'/>
    </xsl:call-template>

    <slide id='foilgroup-{generate-id()}'>
      <xsl:attribute name='master-slide-id'>
        <xsl:choose>
          <xsl:when test='*[not(self::foil|self::foilgroupinfo|self::speakernotes)]'>
            <xsl:value-of select='$masters/apxl:master-slide[@name=$title-only-master]/@id'/>
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select='$masters/apxl:master-slide[@name=$foilgroup-master]/@id'/>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:attribute>

      <drawables>
        <title visibility='tracks-master' vertical-alignment='tracks-master'/>
        <body visibility='hidden' vertical-alignment='tracks-master'/>
        <xsl:call-template name='drawables'/>
      </drawables>
      <transition-style type='inherited'/>
      <thumbnails>
        <thumbnail file='thumbs/st0.tiff' byte-size='6520' size='60 45'/>
      </thumbnails>
      <bullets>
        <bullet marker-type='inherited' level='0'>
          <content tab-stops='L 96' font-size='84' font-color='g1' font-name='GillSans' paragraph-alignment='center'>
            <xsl:apply-templates select='title' mode='slides'/>
          </content>
        </bullet>

        <xsl:apply-templates select='itemizedlist/listitem' mode='slides'/>
      </bullets>
      <xsl:if test='speakernotes'>
        <notes font-size='18' font-name='LucidaGrande'>
          <xsl:apply-templates select='speakernotes/para[1]/node()' mode='slides'/>
          <xsl:for-each select='speakernotes/para[position() != 1]'>
            <xsl:text>; </xsl:text>
            <xsl:apply-templates select='node()' mode='slides'/>
          </xsl:for-each>
        </notes>
      </xsl:if>
    </slide>

    <xsl:apply-templates select='foil' mode='slides'/>

  </xsl:template>

  <xsl:template match='foil' mode='slides'>
    <xsl:variable name='number' select='count(preceding::foilgroup) + count(preceding::foil) + count(preceding-sibling::foil) + 1'/>

    <slide id='foil-{generate-id()}'>
      <xsl:attribute name='master-slide-id'>
        <xsl:choose>
          <xsl:when test='imageobject'>
            <xsl:value-of select='$masters/apxl:master-slide[@name=$title-only-master]/@id'/>
          </xsl:when>
          <xsl:when test='itemizedlist[.//imageobject]'>
            <xsl:value-of select='$masters/apxl:master-slide[@name=$bullet-and-image-master]/@id'/>
          </xsl:when>
          <xsl:when test='itemizedlist'>
            <xsl:value-of select='$masters/apxl:master-slide[@name=$bullet-master]/@id'/>
          </xsl:when>
          <xsl:when test='example|informalexample'>
            <xsl:value-of select='$masters/apxl:master-slide[@name=$title-only-master]/@id'/>
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select='$masters/apxl:master-slide[@name=$bullet-master]/@id'/>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:attribute>
      <drawables>
        <body visibility='tracks-master' vertical-alignment='tracks-master'/>
        <title visibility='tracks-master' vertical-alignment='tracks-master'/>
        <xsl:call-template name='drawables'/>
      </drawables>
      <transition-style type='inherited'/>
      <thumbnails>
        <thumbnail file='thumbs/st0.tiff' byte-size='6520' size='60 45'/>
      </thumbnails>
      <bullets>
        <bullet marker-type='inherited' level='0'>
          <content tab-stops='L 96' font-size='64' font-color='g1' font-name='GillSans' paragraph-alignment='inherited'>
            <!--
            <xsl:apply-templates select='../title' mode='slides'/>
            <xsl:text>: </xsl:text>
-->
            <xsl:apply-templates select='title' mode='slides'/>
          </content>
        </bullet>
        <xsl:apply-templates select='itemizedlist/listitem' mode='slides'/>
      </bullets>
      <xsl:if test='speakernotes'>
        <notes font-size='18' font-name='LucidaGrande'>
          <xsl:apply-templates select='speakernotes/para[1]/node()' mode='slides'/>
          <xsl:for-each select='speakernotes/para[position() != 1]'>
            <xsl:text>; </xsl:text>
            <xsl:apply-templates select='node()' mode='slides'/>
          </xsl:for-each>
        </notes>
      </xsl:if>
    </slide>
  </xsl:template>

  <doc:template xmlns=''>
    <title>drawables Template</title>

    <para>This template adds objects to the drawables section of a foil.  These include images, as well as unadorned (non-bullet) text.</para>

    <para>A single image is placed centered on the foil.  An image on a foil that contains other text is placed on the right-hand-side.</para>
  </doc:template>

  <xsl:template name='drawables'>
    <xsl:choose>
      <xsl:when test='imageobject'>
        <plugin opacity='1' transformation='1 0 0 1 140 130'>
          <plugin-data>
            <plugin:movie bundled='true' src='{imageobject/imagedata/@fileref}' key='root' width='740' height='560'/>
            <string key='CPVersion'>1.0</string>
            <string key='MIMEType'>video/quicktime</string>
          </plugin-data>
          <styles>
            <shadow-style opacity='0' radius='0'/>
          </styles>
        </plugin>
      </xsl:when>
      <xsl:when test='false() and .//informaltable|.//table'>
        <xsl:variable name='table' select='.//informaltable|.//table[1]'/>
        <xsl:variable name='cells' select='$table/tgroup/*/row/entry'/>
        <xsl:variable name='numrows' select='count($table/tgroup/*/row)'/>
        <xsl:variable name='numcols' select='count($table/tgroup/*[1]/row[1]/entry)'/>
        <xsl:variable name='identbase' select='count($table/preceding::node())'/>

        <plugin transformation='1 0 0 1 200 200'>
          <plugin-data>
            <plugin:table key='root' version='1.2'>
              <xsl:attribute name='size'>
                <xsl:text>{800, 400}</xsl:text>
              </xsl:attribute>

              <dict/>
              <xsl:for-each select='$cells'>
                <plugin:element type='text' tr='5834' bl='5838' tl='5833' br='5839'>
                  <xsl:attribute name='tl'>
                    <xsl:value-of select='$identbase + (floor(position() div $numcols) * ($numcols + 1)) + (position() mod $numcols)'/>
                  </xsl:attribute>
                  <xsl:attribute name='tr'>
                    <xsl:value-of select='$identbase + (floor(position() div $numcols) * ($numcols + 1)) + (position() mod $numcols) + 1'/>
                  </xsl:attribute>
                  <xsl:attribute name='bl'>
                    <xsl:value-of select='$identbase + (floor(position() div $numcols) * ($numcols + 1) + 1) + (position() mod $numcols)'/>
                  </xsl:attribute>
                  <xsl:attribute name='br'>
                    <xsl:value-of select='$identbase + (floor(position() div $numcols) * ($numcols + 1) + 1) + (position() mod $numcols) + 1'/>
                  </xsl:attribute>
                  <plugin:node ident='5833'>
                  <xsl:attribute name='pos'>
                    <xsl:text>{0, 300}</xsl:text>
                  </xsl:attribute>
                </plugin:node>
                <plugin:node ident='5834'>
                  <xsl:attribute name='pos'>
                    <xsl:text>{150, 300}</xsl:text>
                  </xsl:attribute>
                </plugin:node>
                <plugin:node ident='5838'>
                  <xsl:attribute name='pos'>
                    <xsl:text>{0, 200}</xsl:text>
                  </xsl:attribute>
                </plugin:node>
                <plugin:node ident='5839'>
                  <xsl:attribute name='pos'>
                    <xsl:text>{150, 200}</xsl:text>
                  </xsl:attribute>
                </plugin:node>
                <content tab-stops='L 84' font-size='32' font-color='g1' font-name='GillSans' paragraph-alignment='center'>
                  <xsl:value-of select='$cells[1]'/>
                </content>
                <dict/>
              </plugin:element>
                
              </xsl:for-each>
            </plugin:table>
          </plugin-data>
        </plugin>
      </xsl:when>
      <xsl:otherwise>
        <xsl:if test='not(self::foilgroup) and .//imageobject'>
          <xsl:variable name='base'>
            <xsl:call-template name='str:substring-after-last'>
              <xsl:with-param name='text' select='.//imageobject/imagedata/@fileref'/>
              <xsl:with-param name='chars' select='"/"'/>
            </xsl:call-template>
          </xsl:variable>
          <image display-name='{$base}' id='image-1' image-data='{.//imageobject/imagedata/@fileref}' byte-size='1' transformation='1 0 0 1 500 200' natural-size='{.//imageobject/imagedata/@width} {.//imageobject/imagedata/@height}' lock-aspect-ratio='true'/>
        </xsl:if>
        <xsl:apply-templates select='para|informalexample|example' mode='slides'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match='itemizedlist/listitem' mode='slides'>
    <bullet marker-type='inherited' level='{count(ancestor::itemizedlist)}'>
      <content tab-stops='L 96' font-size='36' font-color='g1' font-name='GillSans' paragraph-alignment='left'>
        <xsl:apply-templates select='para|informalexample' mode='slides'/>
      </content>
    </bullet>
    <xsl:apply-templates select='itemizedlist/listitem' mode='slides'/>
  </xsl:template>
  <xsl:template match='listitem/para|listitem/informalexample' mode='slides'>
    <xsl:if test='preceding-sibling::*'>
      <xsl:text>

</xsl:text>
    </xsl:if>
    <xsl:apply-templates mode='slides'/>
  </xsl:template>
  <xsl:template match='listitem/informalexample/programlisting' mode='slides'>
    <textbox transformation='1 0 0 1 110 260' size='830 82'>
      <content tab-stops='L 84' font-size='36' font-color='g1' font-name='AmericanTypewriter-CondensedBold' paragraph-alignment='left'>
        <xsl:call-template name='literallayout'/>
      </content>
    </textbox>
  </xsl:template>

  <xsl:template name='literallayout'>
    <xsl:param name='nodes' select='node()'/>
    <xsl:param name='inCDATA' select='false()'/>

    <xsl:choose>
      <xsl:when test='not($nodes) and $inCDATA'>
        <xsl:text disable-output-escaping='yes'>]]&gt;</xsl:text>
      </xsl:when>
      <xsl:when test='not($nodes)'/>
      <xsl:when test='$nodes[1][self::emphasis] and $inCDATA'>
        <xsl:text disable-output-escaping='yes'>]]&gt;</xsl:text>
        <xsl:apply-templates select='$nodes[1]' mode='literal'/>
        <xsl:call-template name='literallayout'>
          <xsl:with-param name='nodes' select='$nodes[position() != 1]'/>
        </xsl:call-template>
      </xsl:when>
      <xsl:when test='$nodes[1][self::emphasis]'>
        <xsl:apply-templates select='$nodes[1]' mode='literal'/>
        <xsl:call-template name='literallayout'>
          <xsl:with-param name='nodes' select='$nodes[position() != 1]'/>
        </xsl:call-template>
      </xsl:when>
      <xsl:when test='$inCDATA'>
        <xsl:apply-templates select='$nodes[1]' mode='literal'/>
        <xsl:call-template name='literallayout'>
          <xsl:with-param name='nodes' select='$nodes[position() != 1]'/>
          <xsl:with-param name='inCDATA' select='$inCDATA'/>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:text disable-output-escaping='yes'>&lt;![CDATA[</xsl:text>
        <xsl:apply-templates select='$nodes[1]' mode='literal'/>
        <xsl:call-template name='literallayout'>
          <xsl:with-param name='nodes' select='$nodes[position() != 1]'/>
          <xsl:with-param name='inCDATA' select='true()'/>
        </xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match='para' mode='slides'>
    <textbox transformation='1 0 0 1 110 {200 + count(preceding-sibling::para|preceding-sibling::informalexample) * 60}' size='830 41'>
      <content tab-stops='L 84' font-size='36' font-color='g1' paragraph-alignment='left'>
        <xsl:attribute name='font-name'>
          <xsl:choose>
            <xsl:when test='@font-style = "italic"'>GillSans-Italic</xsl:when>
            <xsl:otherwise>GillSans</xsl:otherwise>
          </xsl:choose>
        </xsl:attribute>

        <xsl:apply-templates mode='slides'/>
      </content>
    </textbox>
  </xsl:template>

  <xsl:template match='text()' mode='slides'>
    <xsl:variable name='tmp'>
      <xsl:call-template name='str:subst'>
        <xsl:with-param name='text' select='.'/>
        <xsl:with-param name='replace'>&quot;</xsl:with-param>
        <xsl:with-param name='with'>“</xsl:with-param>
      </xsl:call-template>
    </xsl:variable>
    <xsl:variable name='content'>
      <xsl:call-template name='str:subst'>
        <xsl:with-param name='text' select='$tmp'/>
        <xsl:with-param name='replace'>]]&gt;</xsl:with-param>
        <xsl:with-param name='with'>]] &gt;</xsl:with-param>
      </xsl:call-template>
    </xsl:variable>
    <xsl:choose>
      <xsl:when test='ancestor::programlisting'>
        <xsl:value-of disable-output-escaping='yes' select='$content'/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select='$content'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>
  <xsl:template match='text()' mode='literal'>
    <xsl:variable name='tmp'>
      <xsl:call-template name='str:subst'>
        <xsl:with-param name='text' select='.'/>
        <xsl:with-param name='replace'>&lt;</xsl:with-param>
        <xsl:with-param name='with'>&lt;</xsl:with-param>
        <xsl:with-param name='disable-output-escaping' select='yes'/>
      </xsl:call-template>
    </xsl:variable>
    <xsl:variable name='tmp2'>
      <xsl:call-template name='str:subst'>
        <xsl:with-param name='text' select='$tmp'/>
        <xsl:with-param name='replace'>]]&gt;</xsl:with-param>
        <xsl:with-param name='with'>]] &gt;</xsl:with-param>
        <xsl:with-param name='disable-output-escaping' select='yes'/>
      </xsl:call-template>
    </xsl:variable>
    <xsl:value-of select='$tmp2' disable-output-escaping='yes'/>
  </xsl:template>

  <xsl:template match='informalexample|example' mode='slides'>
    <xsl:apply-templates mode='slides'/>
  </xsl:template>
  <xsl:template match='programlisting' mode='slides'>
    <xsl:variable name='lines'>
      <xsl:call-template name='str:count-substring'>
        <xsl:with-param name='text' select='text()'/>
        <xsl:with-param name='chars' select='"&#x0a;"'/>
      </xsl:call-template>
    </xsl:variable>
    <xsl:variable name='offset'>
      <xsl:choose>
        <xsl:when test='string-length(../preceding-sibling::para) > 45'>20</xsl:when>
        <xsl:otherwise>0</xsl:otherwise>
      </xsl:choose>
    </xsl:variable>
    <!-- xsl:comment> layout programlisting with offset <xsl:value-of select='$offset'/> have preceding-sibling para? <xsl:value-of select='count(../preceding-sibling::para)'/> length <xsl:value-of select='string-length(../preceding-sibling::para)'/></xsl:comment -->
    <textbox transformation='1 0 0 1 110 {200 + $offset + count(preceding-sibling::para|../preceding-sibling::para|preceding-sibling::informalexample|preceding-sibling::example) * 60}' size='830 {($lines + 1) * 41}'>     
      <content tab-stops='L 84' font-size='36' font-color='g1' font-name='AmericanTypewriter-CondensedBold' paragraph-alignment='left'>
        <xsl:choose>
          <xsl:when test='emphasis'>
            <xsl:call-template name='literallayout'/>
          </xsl:when>
          <xsl:otherwise>
            <span>
              <xsl:call-template name='literallayout'/>
            </span>
          </xsl:otherwise>
        </xsl:choose>
      </content>
    </textbox>
  </xsl:template>

  <xsl:template match='emphasis' mode='literal'>
    <span>
      <xsl:choose>
        <xsl:when test='ancestor::programlisting and @font-style = "italic" and @font-weight="bold"'>
          <xsl:attribute name='font-name'>AmericanTypewriter-CondensedBoldItalic</xsl:attribute>
        </xsl:when>
        <xsl:when test='@font-style = "italic" and @font-weight="bold"'>
          <xsl:attribute name='font-name'>GillSans-BoldItalic</xsl:attribute>
        </xsl:when>
        <xsl:when test='ancestor::programlisting and @font-style = "italic"'>
          <xsl:attribute name='font-name'>AmericanTypewriter-CondensedItalic</xsl:attribute>
        </xsl:when>
        <xsl:when test='@font-style = "italic"'>
          <xsl:attribute name='font-name'>GillSans-Italic</xsl:attribute>
        </xsl:when>
        <xsl:when test='ancestor::programlisting and @font-weight = "bold"'>
          <xsl:attribute name='font-name'>AmericanTypewriter-CondensedBold</xsl:attribute>
        </xsl:when>
        <xsl:when test='@font-weight = "bold"'>
          <xsl:attribute name='font-name'>GillSans-Bold</xsl:attribute>
        </xsl:when>
      </xsl:choose>
      <xsl:if test='@fill'>
        <xsl:attribute name='font-color'>
          <xsl:variable name='red'>
            <xsl:call-template name='math:cvt-hex-decimal'>
              <xsl:with-param name='value' select='substring(@fill, 2, 2)'/>
            </xsl:call-template>
          </xsl:variable>
          <xsl:variable name='green'>
            <xsl:call-template name='math:cvt-hex-decimal'>
              <xsl:with-param name='value' select='substring(@fill, 4, 2)'/>
            </xsl:call-template>
          </xsl:variable>
          <xsl:variable name='blue'>
            <xsl:call-template name='math:cvt-hex-decimal'>
              <xsl:with-param name='value' select='substring(@fill, 6, 2)'/>
            </xsl:call-template>
          </xsl:variable>

          <xsl:value-of select='$red div 255'/>
          <xsl:text> </xsl:text>
          <xsl:value-of select='$green div 255'/>
          <xsl:text> </xsl:text>
          <xsl:value-of select='$blue div 255'/>
        </xsl:attribute>
      </xsl:if>
      <xsl:call-template name='literallayout'/>
    </span>
  </xsl:template>

  <xsl:template match="*">
    <xsl:copy>
      <xsl:for-each select="@*">
        <xsl:copy/>
      </xsl:for-each>
      <xsl:apply-templates/>
    </xsl:copy>
  </xsl:template>
  <xsl:template match="comment()|processing-instruction()">
    <xsl:copy/>
  </xsl:template>
</xsl:stylesheet>
