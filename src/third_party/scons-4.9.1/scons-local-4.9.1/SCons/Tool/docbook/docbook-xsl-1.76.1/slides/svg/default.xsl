<xsl:stylesheet version='1.0'
  xmlns:xsl='http://www.w3.org/1999/XSL/Transform'
  xmlns:xlink="http://www.w3.org/1999/xlink"
  xmlns:doc='http://nwalsh.com/xsl/documentation/1.0'
  exclude-result-prefixes='doc'>

  <xsl:import href='../../lib/lib.xsl'/>

  <doc:article xmlns=''>
    <articleinfo>
      <title>SVG Slides</title>

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

      <releaseinfo>$Id: default.xsl 6567 2007-01-30 06:43:18Z xmldoc $</releaseinfo>

      <copyright>
        <year>2002</year>
        <holder>Steve Ball, Zveno Pty Ltd</holder>
      </copyright>

      <legalnotice>
        <para>Zveno Pty Ltd makes this software and associated documentation available free of charge for any purpose.  You may make copies of the software but you must include all of this notice on any copy.</para>
        <para>Zveno Pty Ltd does not warrant that this software is error free or fit for any purpose.  Zveno Pty Ltd disclaims any liability for all claims, expenses, losses, damages and costs any user may incur as a result of using, copying or modifying the software.</para>
      </legalnotice>
    </articleinfo>
  </doc:article>

  <xsl:variable name='svg-public-id' select='"-//W3C//DTD SVG 20001102//EN"'/>
  <xsl:variable name='svg-system-id' select='"http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd"'/>

  <xsl:output method="xml" indent='yes' doctype-public='-//W3C//DTD SVG 20001102//EN' doctype-system='http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd' cdata-section-elements="script"/>

  <xsl:strip-space elements='slides foil foilgroup'/>

  <xsl:param name='css-stylesheet'>slides.css</xsl:param>
  <xsl:param name='graphics.dir'>graphics</xsl:param>

  <xsl:param name='toc.bg.color'>white</xsl:param>

  <xsl:param name='font.family'>Arial</xsl:param>
  <xsl:param name='bg.color'>white</xsl:param>
  <xsl:param name='fg.color'>black</xsl:param>

  <xsl:param name='foil.width' select='800'/>
  <xsl:param name='foil.height' select='600'/>

  <xsl:param name='toc.line.max' select='7'/>

  <xsl:attribute-set name="svg.attributes">
    <xsl:attribute name="xml:space">preserve</xsl:attribute>
    <xsl:attribute name="width">100%</xsl:attribute>
    <xsl:attribute name="height"><xsl:value-of select='$foil.height'/></xsl:attribute>
    <xsl:attribute name="style">font-family: <xsl:value-of select='$font.family'/>; font-size: 18pt; fill: <xsl:value-of select='$fg.color'/>; stroke: <xsl:value-of select='$fg.color'/>; background-color: <xsl:value-of select='$bg.color'/></xsl:attribute>
  </xsl:attribute-set>

  <xsl:attribute-set name="text-title">
    <xsl:attribute name="style">font-size: 24pt; font-weight: bold</xsl:attribute>
  </xsl:attribute-set>
  <xsl:attribute-set name="text-author">
    <xsl:attribute name="style">font-size: 18pt</xsl:attribute>
  </xsl:attribute-set>
  <xsl:attribute-set name="text-main">
    <xsl:attribute name="style">font-size: 18pt</xsl:attribute>
  </xsl:attribute-set>

<!-- ============================================================ -->

<xsl:template name="graphics.dir">
  <!-- danger will robinson: template shadows parameter -->
  <xsl:variable name="source.graphics.dir">
    <xsl:call-template name="dbhtml-attribute">
      <xsl:with-param name="pis" select="/processing-instruction('dbhtml')"/>
      <xsl:with-param name="attribute" select="'graphics-dir'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="$source.graphics.dir != ''">
      <xsl:value-of select="$source.graphics.dir"/>
    </xsl:when>
    <xsl:otherwise>
      <xsl:value-of select="$graphics.dir"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template name="css-stylesheet">
  <!-- danger will robinson: template shadows parameter -->
  <xsl:variable name="source.css-stylesheet">
    <xsl:call-template name="dbhtml-attribute">
      <xsl:with-param name="pis" select="/processing-instruction('dbhtml')"/>
      <xsl:with-param name="attribute" select="'css-stylesheet'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="$source.css-stylesheet != ''">
      <xsl:value-of select="$source.css-stylesheet"/>
    </xsl:when>
    <xsl:otherwise>
      <xsl:value-of select="$css-stylesheet"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- ============================================================ -->

  <xsl:template match="/">
    <xsl:apply-templates/>
  </xsl:template>

  <xsl:template match="slides">
    <xsl:if test='$css-stylesheet != ""'>
      <xsl:processing-instruction name='xml-stylesheet'>
        <xsl:text> href="</xsl:text>
        <xsl:value-of select='$css-stylesheet'/>
        <xsl:text>" type="text/css"</xsl:text>
      </xsl:processing-instruction>
    </xsl:if>

    <svg xsl:use-attribute-sets="svg.attributes">
      <title>
        <xsl:value-of select="slidesinfo/title"/>
      </title>

      <defs>
        <xsl:call-template name='svg.defs'/>
      </defs>

      <!-- Create the title foil -->
      <g id='title' display='inline'>
        <xsl:call-template name='render-background'>
          <xsl:with-param name='mode'>title</xsl:with-param>
          <xsl:with-param name='id' select='"title-bg"'/>
        </xsl:call-template>

        <text id='title-main' x='50%' y='33.3%' text-anchor='middle' xsl:use-attribute-sets='text-title'>
          <xsl:value-of select='/slides/slidesinfo/title'/>
        </text>
        <g id='title-author'>
          <xsl:apply-templates select='/slides/slidesinfo/author|/slides/slidesinfo/corpauthor'/>
        </g>

        <set attributeName='display' to='none' attributeType='CSS'>
          <xsl:attribute name='begin'>
            <xsl:text>title.click</xsl:text>
          </xsl:attribute>
          <xsl:attribute name='end'>
            <xsl:text>foil1-previous-button.click; </xsl:text>
            <xsl:value-of select='concat("foil", count(//foil), ".click")'/>
            <xsl:for-each select='//foil'>
              <xsl:value-of select='concat("; foil", count(preceding-sibling::foil|preceding::foil) + 1, "-title-button.click")'/>
            </xsl:for-each>
          </xsl:attribute>
        </set>
      </g>

      <!-- Create the TOC -->
      <xsl:if test='foilgroup'>
        <g id='toc' display='none'>

          <xsl:call-template name='render-background'>
            <xsl:with-param name='mode'>toc</xsl:with-param>
            <xsl:with-param name='id' select='"index-bg"'/>
          </xsl:call-template>

          <text id='toc-main' x='50%' y='50' text-anchor='middle' xsl:use-attribute-sets='text-title'>
            <xsl:value-of select='/slides/slidesinfo/title'/>
          </text>

          <set attributeName='display' to='inline' attributeType='CSS'>
            <xsl:attribute name='begin'>
              <xsl:text>title.click</xsl:text>
              <xsl:for-each select='//foil'>
                <xsl:value-of select='concat("; foil", count(preceding-sibling::foil|preceding::foil) + 1, "-toc-button.click")'/>
              </xsl:for-each>
            </xsl:attribute>
            <xsl:attribute name='end'>
              <xsl:text>toc.click; toc-content.click</xsl:text>
              <xsl:for-each select='//foilgroup'>
                <xsl:value-of select='concat("; index-foilgroup-", count(preceding-sibling::foilgroup|preceding::foilgroup) + 1, ".click")'/>
              </xsl:for-each>
            </xsl:attribute>
          </set>

        </g>
        <g id='toc-content' display='none'>
          <xsl:call-template name='layout-toc-columns'>
            <xsl:with-param name='nodes' select='foilgroup'/>
            <xsl:with-param name='x'>
              <xsl:choose>
                <xsl:when test='count(foilgroup) > $toc.line.max'>
                  <xsl:text>50</xsl:text>
                </xsl:when>
                <xsl:otherwise>75</xsl:otherwise>
              </xsl:choose>
            </xsl:with-param>
          </xsl:call-template>
          <set attributeName='display' to='inline' attributeType='CSS'>
            <xsl:attribute name='begin'>
              <xsl:text>title.click</xsl:text>
              <xsl:for-each select='//foil'>
                <xsl:value-of select='concat("; foil", count(preceding-sibling::foil|preceding::foil) + 1, "-toc-button.click")'/>
              </xsl:for-each>
            </xsl:attribute>
            <xsl:attribute name='end'>
              <xsl:text>toc.click; toc-content.click</xsl:text>
              <xsl:for-each select='//foilgroup'>
                <xsl:value-of select='concat("; index-foilgroup-", count(preceding-sibling::foilgroup|preceding::foilgroup) + 1, ".click")'/>
              </xsl:for-each>
            </xsl:attribute>
          </set>
        </g>
      </xsl:if>

      <xsl:apply-templates select='*[not(self::slidesinfo)]'/>

    </svg>
  </xsl:template>

  <!-- The application is expected to override these templates -->
  <xsl:template name='svg.defs'/>
  <xsl:template name='render-background'>
    <!-- mode lets us know what kind of foil is being produced -->
    <xsl:param name='mode'/>

    <!-- id is a required parameter to include in the generated graphics.
       - This is important for slide transitions.
      -->
    <xsl:param name='id'/>

    <!-- This background covers most of the foil area,
       - but leaves a space in the lower left corner for the
       - controls
      -->

    <xsl:choose>
      <xsl:when test='$mode = "toc"'>
        <g id='{$id}'>
          <!--
          <rect width='{2 * $foil.width}' height='75' style='fill: {$toc.bg.color}; stroke: none'/>
-->
          <rect width="{2 * $foil.width}" height="{$foil.height - 200}" style="fill: {$toc.bg.color}; stroke: none"/>
          <rect transform='translate(100 {$foil.height - 200})' width="{2 * $foil.width}" height="200" style="fill: {$toc.bg.color}; stroke: none"/>
        </g>
      </xsl:when>
      <xsl:otherwise>
        <g id="{$id}">
          <rect width="{2 * $foil.width}" height="{$foil.height - 200}" style="fill: {$bg.color}; stroke: none"/>
          <rect transform='translate(100 {$foil.height - 200})' width="{2 * $foil.width}" height="200" style="fill: {$bg.color}; stroke: none"/>
        </g>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match='author'>
    <text x='50%' y='60%' text-anchor='middle' xsl:use-attribute-sets='text-author'>
      <tspan>
        <xsl:apply-templates select='firstname'/>
      </tspan>
      <tspan>
        <xsl:text> </xsl:text>
      </tspan>
      <tspan>
        <xsl:apply-templates select='surname'/>
      </tspan>
      <xsl:if test='affiliation'>
        <tspan x='0' y='20'>
          <xsl:apply-templates select='affiliation'/>
        </tspan>
      </xsl:if>
    </text>
  </xsl:template>
  <xsl:template match='corpauthor'>
    <text x='50%' y='70%' text-anchor='middle' xsl:use-attribute-sets='text-author'>
      <xsl:apply-templates/>
    </text>
  </xsl:template>

  <xsl:template name='layout-toc-columns'>
    <xsl:param name='nodes'/>
    <xsl:param name='entries-are-links' select='"yes"'/>
    <xsl:param name='highlight'/>
    <xsl:param name='x' select='50'/>

    <xsl:choose>
      <xsl:when test='not($nodes)'/>

      <xsl:otherwise>
        <xsl:for-each select='$nodes[position() &lt;= $toc.line.max]'>
          <g>
            <xsl:if test='$entries-are-links = "yes"'>
              <xsl:attribute name='id'>
                <xsl:text>index-foilgroup-</xsl:text>
                <xsl:value-of select='count(preceding-sibling::foilgroup|preceding::foilgroup) + 1'/>
              </xsl:attribute>
            </xsl:if>
            <text x='{$x}' y='{position() * 35 + 75}' xsl:use-attribute-sets='text-main'>
              <xsl:if test='$highlight and generate-id($highlight) = generate-id(.)'>
                <xsl:attribute name='fill'>#ff8000</xsl:attribute>
                <xsl:attribute name='stroke'>#ff8000</xsl:attribute>
              </xsl:if>
              <xsl:value-of select='title'/>
            </text>
            <xsl:if test='$entries-are-links = "yes"'>
              <set attributeName='fill' attributeType='CSS' to='#ff0033' begin='mouseover' end='mouseout'/>
              <set attributeName='stroke' attributeType='CSS' to='#ff0033' begin='mouseover' end='mouseout'/>
            </xsl:if>
          </g>
        </xsl:for-each>

        <xsl:call-template name='layout-toc-columns'>
          <xsl:with-param name='nodes' select='$nodes[position() > $toc.line.max]'/>
          <xsl:with-param name='entries-are-links' select='$entries-are-links'/>
          <xsl:with-param name='highlight' select='$highlight'/>
          <xsl:with-param name='x' select='$x + 200'/>
        </xsl:call-template>

      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match='foilgroup'>
    <xsl:variable name='fg-num' select='count(preceding-sibling::foilgroup) + 1'/>

    <!-- Add a separator foil to indicate progress -->
    <g id='toc-sep-{$fg-num}' display='none'>
      <xsl:call-template name='render-background'>
        <xsl:with-param name='mode'>toc</xsl:with-param>
        <xsl:with-param name='id' select='concat("toc-sep-", $fg-num, "-bg")'/>
      </xsl:call-template>

      <text id='toc-sep-{$fg-num}-main' x='50%' y='50' text-anchor='middle' xsl:use-attribute-sets='text-title'>
        <xsl:value-of select='/slides/slidesinfo/title'/>
      </text>

      <set attributeName='display' to='inline' attributeType='CSS' end='toc-sep-{$fg-num}.click; toc-sep-{$fg-num}-content.click'>
        <xsl:attribute name='begin'>
          <xsl:choose>
            <xsl:when test='$fg-num = 1'>
              <xsl:text>index-foilgroup-1.click; toc.click; toc-content.click</xsl:text>
            </xsl:when>
            <xsl:otherwise>
              <xsl:text>index-foilgroup-</xsl:text>
              <xsl:value-of select='count(preceding-sibling::foilgroup) + 1'/>
              <xsl:text>.click; foil</xsl:text>
              <xsl:value-of select='count(preceding::foil)'/>
              <xsl:text>.click</xsl:text>
            </xsl:otherwise>
          </xsl:choose>
        </xsl:attribute>
      </set>

    </g>
    <g id='toc-sep-{$fg-num}-content' display='none'>
      <xsl:call-template name='layout-toc-columns'>
        <xsl:with-param name='nodes' select='/slides/foilgroup'/>
        <xsl:with-param name='entries-are-links' select='no'/>
        <xsl:with-param name='highlight' select='.'/>
        <xsl:with-param name='x'>
          <xsl:choose>
            <xsl:when test='count(/slides/foilgroup) > $toc.line.max'>
              <xsl:text>50</xsl:text>
            </xsl:when>
            <xsl:otherwise>75</xsl:otherwise>
          </xsl:choose>
        </xsl:with-param>
      </xsl:call-template>
      <set attributeName='display' to='inline' attributeType='CSS' end='toc-sep-{$fg-num}.click; toc-sep-{$fg-num}-content.click'>
        <xsl:attribute name='begin'>
          <xsl:choose>
            <xsl:when test='$fg-num = 1'>
              <xsl:text>index-foilgroup-1.click; toc.click; toc-content.click</xsl:text>
            </xsl:when>
            <xsl:otherwise>
              <xsl:text>index-foilgroup-</xsl:text>
              <xsl:value-of select='count(preceding-sibling::foilgroup) + 1'/>
              <xsl:text>.click; foil</xsl:text>
              <xsl:value-of select='count(preceding::foil)'/>
              <xsl:text>.click</xsl:text>
            </xsl:otherwise>
          </xsl:choose>
        </xsl:attribute>
      </set>
    </g>

    <!-- still want TOC and Previous buttons -->

    <xsl:apply-templates select='foil'/>
  </xsl:template>

  <xsl:template match='foil'>
    <xsl:variable name='number' select='count(preceding-sibling::foil|preceding::foil) + 1'/>

    <g id='foil{$number}' display='none'>
      <xsl:call-template name='render-background'>
        <xsl:with-param name='id' select='concat("foil", $number, "-bg")'/>
      </xsl:call-template>

      <text id='foil{$number}-title' transform='translate(50 50)'>
        <tspan id='foil{$number}-title-foilgroup' x='0' y='0' xsl:use-attribute-sets='text-title'>
          <xsl:choose>
            <xsl:when test='parent::foilgroup'>
              <xsl:value-of select='../title'/>
            </xsl:when>
            <xsl:otherwise>
              <xsl:value-of select='/slides/slidesinfo/title'/>
            </xsl:otherwise>
          </xsl:choose>
        </tspan>
        <tspan> - </tspan>
        <tspan id='foil{$number}-title-foil' xsl:use-attribute-sets='text-title'>
          <xsl:value-of select='title'/>
        </tspan>
      </text>

      <g transform='translate(50 100)'>
        <xsl:apply-templates select='*[not(self::title)][1]'/>
      </g>

      <xsl:call-template name='foil-events'>
        <xsl:with-param name='number' select='$number'/>
      </xsl:call-template>

    </g>

    <!-- Add previous and TOC buttons 
       - (no need for next, mouse click does that)
      -->

    <g id='foil{$number}-toc-button' transform='translate(20 {$foil.height - 180})' display='none'>
      <g style='opacity: 0'>
        <xsl:call-template name='toc-button'/>
        <text x='25' y='28'>TOC</text>
        <set attributeName='opacity' to='1' attributeType='CSS' begin='mouseover' end='mouseout'/>
      </g>

      <xsl:call-template name='foil-events'>
        <xsl:with-param name='number' select='$number'/>
      </xsl:call-template>

    </g>
    <xsl:if test='$number != 1'>
      <g id='foil{$number}-previous-button' transform='translate(20 {$foil.height - 150})' display='none'>
        <g style='opacity: 0'>
          <xsl:call-template name='previous-button'/>
          <text x='17' y='28'>Previous</text>
          <set attributeName='opacity' to='1' attributeType='CSS' begin='mouseover' end='mouseout'/>
        </g>

        <xsl:call-template name='foil-events'>
          <xsl:with-param name='number' select='$number'/>
        </xsl:call-template>

      </g>
    </xsl:if>

  </xsl:template>

  <!-- The application may override these -->
  <xsl:template name='toc-button'/>
  <xsl:template name='previous-button'/>

  <xsl:template match='foilinfo|foil/title|foil/subtitle|foil/titleabbrev'/>

  <xsl:template name='foil-events'>
    <xsl:param name='number' select='0'/>
    <xsl:param name='attribute' select='"display"'/>
    <xsl:param name='onvalue' select='"inline"'/>
    <xsl:param name='offvalue' select='"none"'/>

    <!-- Must account for first and last foils and also foilgroup separators:
       - On first foil, previous goes back to main TOC,
       - If no TOC foil, then go to title foil instead.
       - First foil in foilgroup follows group separator.
       - Last foil in foilgroup goes to next group separator,
       - except last foil in last group goes to main TOC.
      -->

    <xsl:choose>
      <xsl:when test='$number = 1'>
        <!-- This is the very first foil -->
        <set attributeName='{$attribute}' to='{$onvalue}' attributeType='CSS'
            end='foil{$number}.click; foil{$number}-toc-button.click'>
          <xsl:attribute name='begin'>
            <xsl:choose>
              <xsl:when test='parent::foilgroup'>
                <xsl:value-of select='concat("toc-sep-", count(preceding::foilgroup) + 1, ".click")'/>
              </xsl:when>
              <xsl:otherwise>
                <xsl:text>title.click</xsl:text>
              </xsl:otherwise>
            </xsl:choose>
            <xsl:if test='parent::foilgroup and not(preceding-sibling::foil)'>
              <xsl:value-of select='concat("; index-foilgroup-", count(preceding::foilgroup) + 1, ".click")'/>
            </xsl:if>
            <xsl:value-of select='concat("; foil", $number + 1, "-previous-button.click")'/>
          </xsl:attribute>
        </set>
      </xsl:when>
      <xsl:when test='count(following-sibling::foil|following::foil) = 0'>
        <!-- This is the very last foil -->
        <set attributeName='{$attribute}' to='{$onvalue}' attributeType='CSS'
            end='foil{$number}.click; foil{$number}-toc-button.click; foil{$number}-previous-button.click'>
          <xsl:attribute name='begin'>
            <xsl:choose>
              <xsl:when test='parent::foilgroup and not(preceding-sibling::foil)'>
                <xsl:value-of select='concat("; index-foilgroup-", count(preceding::foilgroup) + 1, ".click; toc-sep-", count(preceding::foilgroup) + 1, ".click")'/>
              </xsl:when>
              <xsl:otherwise>
                <xsl:value-of select='concat("foil", $number - 1, ".click")'/>
              </xsl:otherwise>
            </xsl:choose>
          </xsl:attribute>
        </set>
      </xsl:when>
      <xsl:otherwise>
        <set attributeName='{$attribute}' to='{$onvalue}' attributeType='CSS'
            end='foil{$number}.click; foil{$number}-toc-button.click; foil{$number}-previous-button.click'>
          <xsl:attribute name='begin'>
            <xsl:value-of select='concat("foil", $number + 1, "-previous-button.click")'/>
            <xsl:choose>
              <xsl:when test='parent::foilgroup and not(preceding-sibling::foil)'>
                <xsl:value-of select='concat("; index-foilgroup-", count(preceding::foilgroup) + 1, ".click; toc-sep-", count(preceding::foilgroup) + 1, ".click")'/>
              </xsl:when>
              <xsl:otherwise>
                <xsl:value-of select='concat("; foil", $number - 1, ".click")'/>
              </xsl:otherwise>
            </xsl:choose>
          </xsl:attribute>
        </set>
      </xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <xsl:template match="para">
    <xsl:variable name='depth'>
      <xsl:choose>
        <xsl:when test='@depth'>
          <xsl:value-of select='@depth'/>
        </xsl:when>
        <xsl:otherwise>
          <xsl:text>1</xsl:text>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:variable>

    <g transform='translate(0 30)'>
      <xsl:if test='not(@style)'>
        <g transform='translate({25 * ($depth - 1)} 0)'>
          <xsl:call-template name='bullet'/>
        </g>
      </xsl:if>

      <text y="10">
        <xsl:attribute name='x'>
          <xsl:value-of select='25 * $depth'/>
        </xsl:attribute>
        <xsl:if test='@style'>
          <xsl:attribute name='style'>
            <xsl:value-of select='@style'/>
          </xsl:attribute>
        </xsl:if>

        <xsl:apply-templates/>
      </text>

      <xsl:apply-templates select='following-sibling::*[1]'/>
    </g>
  </xsl:template>

  <xsl:template match='text()'>
    <tspan>
      <xsl:value-of select='.'/>
    </tspan>
  </xsl:template>

  <xsl:template match='emphasis'>
    <xsl:variable name='style'>
      <xsl:choose>
        <xsl:when test='@role = "bold"'>
          <xsl:text>font-weight: bold</xsl:text>
        </xsl:when>
        <xsl:when test='@role = "italic"'>
          <xsl:text>font-style: italic</xsl:text>
        </xsl:when>
        <xsl:otherwise>
          <xsl:text>font-style: italic</xsl:text>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:variable>

    <tspan style='{$style}'>
      <xsl:value-of select='.'/>
    </tspan>
  </xsl:template>

  <xsl:template match='listitem'>
    <xsl:call-template name="bullet"/>
    <g transform='translate(20 0)'>
      <xsl:apply-templates/>
    </g>
  </xsl:template>

  <xsl:template name="bullet">
    <xsl:choose>
      <xsl:when test="@depth = 1 or count(ancestor-or-self::listitem) = 1">
        <xsl:call-template name="large-filled-circle"/>
      </xsl:when>
      <xsl:when test="@depth = 2 or count(ancestor-or-self::listitem) = 2">
        <xsl:call-template name="small-open-circle"/>
      </xsl:when>
      <xsl:when test="@depth = 3 or count(ancestor-or-self::listitem) = 3">
        <xsl:call-template name="small-filled-circle"/>
      </xsl:when>
      <xsl:when test="@depth = 4 or count(ancestor-or-self::listitem) = 4">
        <xsl:call-template name="closed-toggle"/>
      </xsl:when>
      <xsl:when test="@depth = 5 or count(ancestor-or-self::listitem) = 5">
        <xsl:call-template name="large-filled-circle"/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="small-open-box"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template name="closed-toggle">
    <polyline fill="white" stroke="black" stroke-width="1" points="0 0 10 5 0 10 0 0"/>
  </xsl:template>
  <xsl:template name="large-filled-circle">
    <circle fill="black" cx="10" cy="6" r="5"/>
  </xsl:template>
  <xsl:template name="small-filled-circle">
    <circle fill="black" cx="10" cy="6" r="2.5"/>
  </xsl:template>
  <xsl:template name="small-open-circle">
    <circle fill="none" stroke="black" stroke-width="1" cx="10" cy="6" r="2.5"/>
  </xsl:template>
  <xsl:template name="small-open-box">
    <rect fill="none" stroke="black" stroke-width="1" x="5" y="5" width="5" height="5"/>
  </xsl:template>

  <xsl:template match="subject">
    <tspan style="font-weight: bold">
      <xsl:apply-templates/>
    </tspan>
  </xsl:template>

  <xsl:template match="informalexample">
  </xsl:template>
  <xsl:template match="programlisting">
    <!-- Output lines verbatim -->
  </xsl:template>

  <xsl:template match="imageobject|mediaobject">
    <xsl:apply-templates/>
  </xsl:template>
  <xsl:template match='textobject|videoobject'/>
  <xsl:template match='imagedata'>
    <g transform='translate(0 30)'>
      <image xlink:href='{@fileref}' x='0' y='0' width='600' height='400'/>
    </g>
  </xsl:template>

  <xsl:template match='ulink'>
    <a xlink:href='{@url}'>
      <xsl:apply-templates/>
    </a>
  </xsl:template>

</xsl:stylesheet>

