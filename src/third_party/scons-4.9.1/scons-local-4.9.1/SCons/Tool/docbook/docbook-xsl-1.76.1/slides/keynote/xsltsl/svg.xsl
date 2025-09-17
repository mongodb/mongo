<xsl:stylesheet version='1.0'
  xmlns:xsl='http://www.w3.org/1999/XSL/Transform'
  xmlns:svg='http://xsltsl.org/svg'
  xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
  exclude-result-prefixes="doc">

  <xsl:variable name='svg-public-id' select='"-//W3C//DTD SVG 20010904//EN"'/>
  <xsl:variable name='svg-system-id' select='"http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd"'/>

  <doc:reference xmlns="">
    <referenceinfo>
      <releaseinfo role="meta">
        $Id: svg.xsl 3991 2004-11-10 06:51:55Z balls $
      </releaseinfo>
      <author>
        <surname>Ball</surname>
        <firstname>Steve</firstname>
      </author>
      <copyright>
        <year>2002</year>
        <holder>Steve Ball</holder>
      </copyright>
    </referenceinfo>

    <title>SVG Stylesheet</title>

    <partintro>
      <section>
        <title>Introduction</title>

        <para>This module provides templates for creating SVG images.</para>
      </section>
    </partintro>
  </doc:reference>

  <doc:template name="svg:aqua-button-defs" xmlns="">
    <refpurpose>Aqua-style Button</refpurpose>

    <refdescription>
      <para>Part of the mechanism to create an Aqua-style button.  Include a call to this template in your SVG document's <sgmltag>defs</sgmltag> element.  This template only needs to be included once.  Use this in conjunction with <sgmltag>svg:aqua-button</sgmltag>.</para>

      <para>The default values for color1, color2 and color3 result in a grey button.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>prefix</term>
          <listitem>
            <para>A prefix to append to the identifiers used, so that they don't clash with other identifiers.  Default: "aqua-".</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>color1</term>
          <listitem>
            <para>The base colour of the button.  Default: "#d9d9d9".</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>color2</term>
          <listitem>
            <para>A "background" colour for the button.  Should be a darker colour than color1.  Default: "#a9a9a9".</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>color3</term>
          <listitem>
            <para>A highlight colour for the button.  Should be a lighter colour than color1.  Default: "#f9f9f9".</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns SVG result-tree-fragment.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="svg:aqua-button-defs">
    <xsl:param name="prefix" select='"aqua-"'/>
    <xsl:param name="color1" select='"#d9d9d9"'/>
    <xsl:param name="color2" select='"#a9a9a9"'/>
    <xsl:param name="color3" select='"#f9f9f9"'/>

    <linearGradient id='{$prefix}gradient' x1='0%' y1='0%' x2='0%' y2='100%'>
      <stop offset='0%' stop-color='{$color2}'/>
      <stop offset='100%' stop-color='{$color1}'/>
    </linearGradient>
    <linearGradient id='{$prefix}highlight-gradient' x1='0%' y1='0%' x2='0%' y2='100%'>
      <stop offset='0%' stop-color='#ffffff'/>
      <stop offset='75%' stop-color='{$color3}' stop-opacity='0'/>
      <stop offset='100%' stop-color='{$color3}' stop-opacity='0'/>
    </linearGradient>
    <linearGradient id='{$prefix}revhighlight-gradient' x1='0%' y1='100%' x2='0%' y2='0%'>
      <stop offset='0%' stop-color='#ffffff'/>
      <stop offset='50%' stop-color='{$color3}' stop-opacity='0'/>
      <stop offset='100%' stop-color='{$color3}' stop-opacity='0'/>
    </linearGradient>
    <linearGradient id='{$prefix}corner-left-gradient' x1='0%' y1='0%' x2='100%' y2='100%'>
      <stop offset='0%' stop-color='#000000'/>
      <stop offset='100%' stop-color='{$color3}' stop-opacity='0'/>
    </linearGradient>
    <linearGradient id='{$prefix}corner-right-gradient' x1='100%' y1='0%' x2='0%' y2='100%'>
      <stop offset='0%' stop-color='#000000'/>
      <stop offset='100%' stop-color='{$color3}' stop-opacity='0'/>
    </linearGradient>

    <filter id='{$prefix}filter-blur' filterUnits='userSpaceOnUse' x='0' y='0' width='200' height='100'>
      <feGaussianBlur in='SourceGraphic' stdDeviation='2'/>
    </filter>
    <filter id='{$prefix}drop-shadow' y='-5' height='100'>
      <feColorMatrix type='matrix' in='SourceAlpha' result='inglow'
	values='.5 .5 .5 1 0
		.5 .5 .5 1 0
		.5 .5 .5 1 0
		0 0 0 1 0'/>
      <feBlend mode='multiply' in2='SourceGraphic' in='inglow' result='innerglow'/>

      <feGaussianBlur stdDeviation='4' in='SourceAlpha' result='shadow'/>
      <feColorMatrix type='matrix' in='shadow' result='lightshadow'
	values='.33 .33 .33 1 0
		.33 .33 .33 1 0
		.33 .33 .33 1 0
		0 0 0 1 0'/>
      <feOffset in='lightshadow' dx='0' dy='3' result='dropshadow'/>
      <feMerge>
	<feMergeNode in='dropshadow'/>
	<feMergeNode in='innerglow'/>
      </feMerge>
    </filter>

  </xsl:template>

  <doc:template name="svg:aqua-button" xmlns="">
    <refpurpose>Aqua-style Button</refpurpose>

    <refdescription>
      <para>Part of the mechanism to create an Aqua-style button.  Include a call to this template in your SVG document where you want a button to appear.  This template can be used many times in a single SVG document.  Use this in conjunction with <sgmltag>svg:aqua-button-defs</sgmltag>.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>prefix</term>
          <listitem>
            <para>A prefix to append to the identifiers used, so that they don't clash with other identifiers.  Default: "aqua-".</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns SVG result-tree-fragment.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='svg:aqua-button'>
    <xsl:param name="prefix" select='"aqua-"'/>

    <g filter='url(#{$prefix}drop-shadow)'>
      <clipPath id='{$prefix}main-shape'>
        <path clip-rule='evenodd'
                d="M50,90 C0,90 0,30 50,30 L150,30 C200,30 200,90 150,90 z" />
      </clipPath>
      <path fill="url(#{$prefix}gradient)" stroke="none"
                d="M50,90 C0,90 0,30 50,30 L150,30 C200,30 200,90 150,90 z" />
      <path clip-path='url(#{$prefix}main-shape)' fill='url(#{$prefix}corner-left-gradient)' stroke='none' filter='url(#{$prefix}filter-blur)'
                d="M50,57 L13,57 A35,35 -90 0,1 50,30 z" />
      <path clip-path='url(#{$prefix}main-shape)' fill='url(#{$prefix}corner-right-gradient)' stroke='none' filter='url(#{$prefix}filter-blur)'
                d="M150,30 A35,35 90 0,1 190,57 L150,57 z" />
      <path fill="url(#{$prefix}highlight-gradient)" stroke="none" stroke-width='1'
                d="M50,65 C20,65 20,35 50,35 L150,35 C180,35 180,65 150,65 z" />
      <path filter='url(#{$prefix}filter-blur)' fill="url(#{$prefix}revhighlight-gradient)" stroke="none"
                d="M50,85 C10,85 10,35 50,35 L150,35 C190,35 190,85 150,85 z" />
    </g>
  </xsl:template>
</xsl:stylesheet>
