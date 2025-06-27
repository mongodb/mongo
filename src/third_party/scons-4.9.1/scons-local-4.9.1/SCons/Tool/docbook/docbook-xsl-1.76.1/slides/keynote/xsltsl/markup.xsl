<xsl:stylesheet version='1.0'
  xmlns:xsl='http://www.w3.org/1999/XSL/Transform'
  xmlns:doc='http://xsltsl.org/xsl/documentation/1.0'
  xmlns:markup='http://xsltsl.org/markup'
  xmlns:str='http://xsltsl.org/string'
  extension-element-prefixes='doc markup str'>

  <doc:reference xmlns=''>
    <referenceinfo>
      <releaseinfo role="meta">
	$Id: markup.xsl 3991 2004-11-10 06:51:55Z balls $
      </releaseinfo>
      <author>
	<surname>Ball</surname>
	<firstname>Steve</firstname>
      </author>
      <copyright>
	<year>2003</year>
	<year>2001</year>
	<holder>Steve Ball</holder>
      </copyright>
    </referenceinfo>

    <title>XML Markup Templates</title>

    <partintro>
      <section>
	<title>Introduction</title>

	<para>This stylesheet module provides functions for generating literal XML markup.</para>

      </section>
    </partintro>

  </doc:reference>

  <doc:template name="markup:xml-declaration" xmlns="">
    <refpurpose>Create an XML Declaration</refpurpose>

    <refdescription>
      <para>This template returns an XML Declaration.  Although the XSLT standard provides control over the generation of the XML Declaration, this template may be useful in circumstances where the values must be computed at runtime.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>version</term>
	  <listitem>
	    <para>Version number.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>standalone</term>
	  <listitem>
	    <para>Standalone indication.  Must be value "yes" or "no".</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>encoding</term>
	  <listitem>
	    <para>Character encoding.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns an XML Declaration as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:xml-declaration'>
    <xsl:param name='version' select="'1.0'"/>
    <xsl:param name='standalone'/>
    <xsl:param name='encoding'/>

    <xsl:text disable-output-escaping='yes'>&lt;?xml version="</xsl:text>
    <xsl:copy-of select="$version"/>
    <xsl:text>"</xsl:text>

    <xsl:choose>
      <xsl:when test="string-length($standalone) = 0"/>
      <xsl:when test='$standalone = "yes" or $standalone = "no"'>
        <xsl:text> standalone="</xsl:text>
        <xsl:copy-of select="$standalone"/>
        <xsl:text>"</xsl:text>
      </xsl:when>
      <xsl:otherwise>
        <xsl:message terminate="yes">invalid value "<xsl:value-of select="$standalone"/>" for standalone attribute</xsl:message>
      </xsl:otherwise>
    </xsl:choose>

    <xsl:if test='string-length($encoding) &gt; 0'>
      <xsl:text> encoding="</xsl:text>
      <xsl:copy-of select='$encoding'/>
      <xsl:text>"</xsl:text>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>?&gt;
</xsl:text>
  </xsl:template>

  <doc:template name="markup:doctype-declaration" xmlns="">
    <refpurpose>Create a Document Type Declaration</refpurpose>

    <refdescription>
      <para>This template returns a Document Type Declaration.  Although the XSLT standard provides control over the generation of a Document Type Declaration, this template may be useful in circumstances where the values for the identifiers or the internal subset must be computed at runtime.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>docel</term>
	  <listitem>
	    <para>The name of the document element.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>publicid</term>
	  <listitem>
	    <para>The public identifier for the external DTD subset.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>systemid</term>
	  <listitem>
	    <para>The system identifier for the external DTD subset.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>internaldtd</term>
	  <listitem>
	    <para>The internal DTD subset.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a Document Type Declaration as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:doctype-declaration'>
    <xsl:param name='docel'/>
    <xsl:param name='publicid'/>
    <xsl:param name='systemid'/>
    <xsl:param name='internaldtd'/>

    <xsl:if test='string-length($docel) = 0'>
      <xsl:message terminate='yes'>No document element specified</xsl:message>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>&lt;!DOCTYPE </xsl:text>
    <xsl:copy-of select="$docel"/>

    <xsl:call-template name='markup:external-identifier'>
      <xsl:with-param name='publicid' select='$publicid'/>
      <xsl:with-param name='systemid' select='$systemid'/>
      <xsl:with-param name='leading-space' select='true()'/>
    </xsl:call-template>

    <xsl:if test='string-length($internaldtd) &gt; 0'>
      <xsl:text> [</xsl:text>
      <xsl:copy-of select='$internaldtd'/>
      <xsl:text>]</xsl:text>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>&gt;
</xsl:text>
  </xsl:template>

  <doc:template name="markup:element-declaration" xmlns="">
    <refpurpose>Create an Element Declaration</refpurpose>

    <refdescription>
      <para>This template returns an element declaration..</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>type</term>
	  <listitem>
	    <para>The element type.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>content-spec</term>
	  <listitem>
	    <para>The content specification.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns an element declaration as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:element-declaration'>
    <xsl:param name='type'/>
    <xsl:param name='content-spec' select="'ANY'"/>

    <xsl:if test='string-length($type) = 0'>
      <xsl:message terminate='yes'>element type must be specified</xsl:message>
    </xsl:if>
    <xsl:if test='string-length($content-spec) = 0'>
      <xsl:message terminate='yes'>content specification must be specified</xsl:message>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>&lt;!ELEMENT </xsl:text>
    <xsl:copy-of select='$type'/>
    <xsl:text> </xsl:text>
    <xsl:copy-of select='$content-spec'/>
    <xsl:text disable-output-escaping='yes'>&gt;</xsl:text>
  </xsl:template>

  <doc:template name="markup:attlist-declaration" xmlns="">
    <refpurpose>Create an Attribute List Declaration</refpurpose>

    <refdescription>
      <para>This template returns an attribute list declaration.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>type</term>
	  <listitem>
	    <para>The element type.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>attr-defns</term>
	  <listitem>
	    <para>Attribute definitions.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns an attribute list declaration as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:attlist-declaration'>
    <xsl:param name='type'/>
    <xsl:param name='attr-defns'/>

    <xsl:if test='string-length($type) = 0'>
      <xsl:message terminate='yes'>element type must be specified</xsl:message>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>&lt;!ATTLIST </xsl:text>
    <xsl:copy-of select='$type'/>
    <xsl:text> </xsl:text>
    <xsl:copy-of select='$attr-defns'/>
    <xsl:text disable-output-escaping='yes'>&gt;</xsl:text>
  </xsl:template>

  <doc:template name="markup:attribute-definition" xmlns="">
    <refpurpose>Create an Attribute Definition</refpurpose>

    <refdescription>
      <para>This template returns an attribute definition.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>name</term>
	  <listitem>
	    <para>The attribute name.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>type</term>
	  <listitem>
	    <para>The attribute type.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>default</term>
	  <listitem>
	    <para>The attribute default.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns an attribute definition as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:attribute-definition'>
    <xsl:param name='name'/>
    <xsl:param name='type'/>
    <xsl:param name='default'/>

    <xsl:if test='string-length($name) = 0'>
      <xsl:message terminate='yes'>attribute name must be specified</xsl:message>
    </xsl:if>
    <xsl:if test='string-length($type) = 0'>
      <xsl:message terminate='yes'>attribute type must be specified</xsl:message>
    </xsl:if>
    <xsl:if test='string-length($default) = 0'>
      <xsl:message terminate='yes'>attribute default must be specified</xsl:message>
    </xsl:if>

    <xsl:text> </xsl:text>
    <xsl:copy-of select='$name'/>
    <xsl:text> </xsl:text>
    <xsl:copy-of select='$type'/>
    <xsl:text> </xsl:text>
    <xsl:copy-of select='$default'/>
  </xsl:template>

  <doc:template name="markup:entity-declaration" xmlns="">
    <refpurpose>Create an Entity Declaration</refpurpose>

    <refdescription>
      <para>This template returns an entity declaration.</para>
      <para>If the 'text' parameter is given a value, then an internal entity is created.  If either the 'publicid' or 'systemid' parameters are given a value then an external entity is created.  It is an error for the 'text' parameter to have value as well as the 'publicid', 'systemid' or 'notation' parameters.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>name</term>
	  <listitem>
	    <para>The entity name.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>parameter</term>
	  <listitem>
	    <para>Boolean value to determine whether a parameter entity is created.  Default is 'false()'.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The replacement text.  Must be a string.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>nodes</term>
	  <listitem>
	    <para>The replacement text as a nodeset.  The nodeset is formatted as XML using the as-xml template.  If both text and nodes are specified then nodes takes precedence.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>publicid</term>
	  <listitem>
	    <para>The public identifier for an external entity.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>systemid</term>
	  <listitem>
	    <para>The system identifier for an external entity.</para>
	  </listitem>
	</varlistentry>
 	<varlistentry>
	  <term>notation</term>
	  <listitem>
	    <para>The notation for an external entity.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns an entity declaration as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:entity-declaration'>
    <xsl:param name='name'/>
    <xsl:param name='parameter' select='false()'/>
    <xsl:param name='text'/>
    <xsl:param name='nodes'/>
    <xsl:param name='publicid'/>
    <xsl:param name='systemid'/>
    <xsl:param name='notation'/>

    <xsl:if test='string-length($name) = 0'>
      <xsl:message terminate='yes'>entity name must be specified</xsl:message>
    </xsl:if>
    <xsl:if test='string-length($text) &gt; 0 and 
                  (string-length($publicid) &gt; 0 or
                   string-length($systemid) &gt; 0 or
                   string-length($notation) &gt; 0)'>
      <xsl:message terminate='yes'>both replacement text and external identifier specified</xsl:message>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>&lt;!ENTITY </xsl:text>
    <xsl:copy-of select='$name'/>
    <xsl:text> </xsl:text>
    <xsl:if test="$parameter">
      <xsl:text>% </xsl:text>
    </xsl:if>

    <xsl:choose>
      <xsl:when test="$nodes">
        <xsl:call-template name='markup:quote-value'>
          <xsl:with-param name='value'>
            <xsl:call-template name="markup:as-xml">
              <xsl:with-param name="nodes" select="$nodes"/>
            </xsl:call-template>
          </xsl:with-param>
        </xsl:call-template>
      </xsl:when>
      <xsl:when test='$text'>
        <xsl:call-template name='markup:quote-value'>
          <xsl:with-param name='value' select='$text'/>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name='markup:external-identifier'>
          <xsl:with-param name='publicid' select='$publicid'/>
          <xsl:with-param name='systemid' select='$systemid'/>
        </xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>

    <xsl:if test='$notation'>
      <xsl:text> NDATA "</xsl:text>
      <xsl:copy-of select='$notation'/>
      <xsl:text>"</xsl:text>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>&gt;</xsl:text>
  </xsl:template>

  <doc:template name="markup:quote-value" xmlns="">
    <refpurpose>Quote an Attribute Value</refpurpose>

    <refdescription>
      <para>This template returns a quoted value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>value</term>
	  <listitem>
	    <para>The value to quote.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a quote value as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:quote-value'>
    <xsl:param name='value'/>

    <xsl:variable name="quoted">
      <xsl:call-template name='markup:quote-value-aux'>
        <xsl:with-param name='value' select='$value'/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:choose>
      <xsl:when test="contains($value, '&lt;')">
        <xsl:call-template name='str:subst'>
          <xsl:with-param name='text' select='$quoted'/>
          <xsl:with-param name='replace'>&lt;</xsl:with-param>
          <xsl:with-param name='with'>
            <xsl:text disable-output-escaping='yes'>&amp;lt;</xsl:text>
          </xsl:with-param>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:copy-of select='$quoted'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template name='markup:quote-value-aux'>
    <xsl:param name='value'/>

    <!-- Quoting hell! -->
    <xsl:variable name="quot">&quot;</xsl:variable>
    <xsl:variable name="apos">&apos;</xsl:variable>

    <xsl:choose>
      <xsl:when test='contains($value, $quot) and contains($value, $apos)'>
        <xsl:text>"</xsl:text>
        <xsl:call-template name='str:subst'>
          <xsl:with-param name='text' select='$value'/>
          <xsl:with-param name='replace'>"</xsl:with-param>
          <xsl:with-param name='with'>
            <xsl:text disable-output-escaping='yes'>&amp;quot;</xsl:text>
          </xsl:with-param>
        </xsl:call-template>
        <xsl:text>"</xsl:text>
      </xsl:when>
      <xsl:when test='contains($value, $quot)'>
        <xsl:text>'</xsl:text>
        <xsl:value-of select='$value'/>
        <xsl:text>'</xsl:text>
      </xsl:when>
      <xsl:otherwise>
        <xsl:text>"</xsl:text>
        <xsl:value-of select='$value'/>
        <xsl:text>"</xsl:text>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="markup:external-identifier" xmlns="">
    <refpurpose>Create an External Identifier</refpurpose>

    <refdescription>
      <para>This template returns an external identifier.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>publicid</term>
	  <listitem>
	    <para>The public identifier.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>systemid</term>
	  <listitem>
	    <para>The system identifier.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns an external identifier as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:external-identifier'>
    <xsl:param name='publicid'/>
    <xsl:param name='systemid'/>
    <xsl:param name='leading-space' select='false()'/>

    <xsl:choose>
      <xsl:when test='string-length($publicid) &gt; 0'>
        <xsl:if test='$leading-space'>
          <xsl:text> </xsl:text>
        </xsl:if>
        <xsl:text disable-output-escaping='yes'>PUBLIC "</xsl:text>
        <xsl:value-of select='$publicid' disable-output-escaping='yes'/>
        <xsl:text disable-output-escaping='yes'>"</xsl:text>
        <xsl:if test='string-length($systemid) &gt; 0'>
          <xsl:text disable-output-escaping='yes'> "</xsl:text>
          <xsl:value-of select='$systemid' disable-output-escaping='yes'/>
          <xsl:text disable-output-escaping='yes'>"</xsl:text>
        </xsl:if>
      </xsl:when>
      <xsl:when test="string-length($systemid) &gt; 0">
        <xsl:if test='$leading-space'>
          <xsl:text> </xsl:text>
        </xsl:if>
        <xsl:text disable-output-escaping='yes'>SYSTEM "</xsl:text>
        <xsl:value-of select='$systemid' disable-output-escaping='yes'/>
        <xsl:text disable-output-escaping='yes'>"</xsl:text>
      </xsl:when>
    </xsl:choose>
  </xsl:template>

  <doc:template name="markup:entity-reference" xmlns="">
    <refpurpose>Create an Entity Reference</refpurpose>

    <refdescription>
      <para>This template returns an entity reference.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>name</term>
	  <listitem>
	    <para>The name of the entity.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns an entity reference as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:entity-reference'>
    <xsl:param name='name'/>

    <xsl:text disable-output-escaping='yes'>&amp;</xsl:text>
    <xsl:value-of select='$name'/>
    <xsl:text>;</xsl:text>

  </xsl:template>

  <doc:template name="markup:notation-declaration" xmlns="">
    <refpurpose>Create a Notation Declaration</refpurpose>

    <refdescription>
      <para>This template returns a notation declaration.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>name</term>
	  <listitem>
	    <para>The notation name.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>publicid</term>
	  <listitem>
	    <para>The public identifier for the notation.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>systemid</term>
	  <listitem>
	    <para>The system identifier for the notation.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a notation declaration as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:notation-declaration'>
    <xsl:param name='name'/>
    <xsl:param name='publicid'/>
    <xsl:param name='systemid'/>

    <xsl:if test='string-length($name) = 0'>
      <xsl:message terminate='yes'>notation name must be specified</xsl:message>
    </xsl:if>
    <xsl:if test='string-length($publicid) = 0 and string-length($systemid) = 0'>
      <xsl:message terminate='yes'>external identifier must be specified</xsl:message>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>&lt;!NOTATION </xsl:text>
    <xsl:copy-of select='$name'/>

    <xsl:call-template name='markup:external-identifier'>
      <xsl:with-param name='publicid' select='$publicid'/>
      <xsl:with-param name='systemid' select='$systemid'/>
      <xsl:with-param name='leading-space' select='true()'/>
    </xsl:call-template>

    <xsl:text disable-output-escaping='yes'>&gt;</xsl:text>
  </xsl:template>

  <doc:template name="markup:cdata-section" xmlns="">
    <refpurpose>Create a CDATA Section</refpurpose>

    <refdescription>
      <para>This template returns a CDATA Section.  The XSLT specification provides a mechanism for instructing the XSL processor to output character data in a CDATA section for certain elements, but this template may be useful in those circumstances where not all instances of an element are to have their content placed in a CDATA section.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>text</term>
	  <listitem>
	    <para>The content of the CDATA section.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a CDATA section as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:cdata-section'>
    <xsl:param name='text'/>

    <xsl:if test="contains($text, ']]&gt;')">
      <xsl:message terminate="yes">CDATA section contains "]]&gt;"</xsl:message>
    </xsl:if>

    <xsl:text disable-output-escaping='yes'>&lt;![CDATA[</xsl:text>
    <xsl:copy-of select='$text'/>
    <xsl:text disable-output-escaping='yes'>]]&gt;</xsl:text>
  </xsl:template>

  <doc:template name="markup:as-xml" xmlns="">
    <refpurpose>Format Nodeset As XML Markup</refpurpose>

    <refdescription>
      <para>This template returns XML markup.  Each node in the given nodeset is converted to its equivalent XML markup.</para>

      <para>BUG: This version may not adequately handle XML Namespaces.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>nodes</term>
	  <listitem>
	    <para>Nodeset to format as XML.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns XML markup.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='markup:as-xml'>
    <xsl:param name='nodes'/>

    <xsl:if test="$nodes">
      <xsl:choose>
        <xsl:when test="$nodes[1]/self::*">
          <xsl:text disable-output-escaping='yes'>&lt;</xsl:text>
          <xsl:value-of select="name($nodes[1])"/>
          <xsl:for-each select="$nodes[1]/@*">
            <xsl:text> </xsl:text>
            <xsl:value-of select="name()"/>
            <xsl:text>=</xsl:text>
            <xsl:call-template name='markup:quote-value'>
              <xsl:with-param name='value' select='.'/>
            </xsl:call-template>
          </xsl:for-each>

          <xsl:choose>
            <xsl:when test='$nodes[1]/node()'>
              <xsl:text disable-output-escaping='yes'>&gt;</xsl:text>
              <xsl:call-template name='markup:as-xml'>
                <xsl:with-param name='nodes' select='$nodes[1]/node()'/>
              </xsl:call-template>
              <xsl:text disable-output-escaping='yes'>&lt;/</xsl:text>
              <xsl:value-of select="name($nodes[1])"/>
              <xsl:text disable-output-escaping='yes'>&gt;</xsl:text>
            </xsl:when>
            <xsl:otherwise>
              <xsl:text disable-output-escaping='yes'>/&gt;</xsl:text>
            </xsl:otherwise>
          </xsl:choose>
        </xsl:when>
        <xsl:when test="$nodes[1]/self::text()">
          <xsl:value-of select="$nodes[1]"/>
        </xsl:when>
        <xsl:when test="$nodes[1]/self::comment()">
          <xsl:text disable-output-escaping='yes'>&lt;!--</xsl:text>
          <xsl:value-of select="$nodes[1]"/>
          <xsl:text disable-output-escaping='yes'>--&gt;</xsl:text>
        </xsl:when>
        <xsl:when test="$nodes[1]/self::processing-instruction()">
          <xsl:text disable-output-escaping='yes'>&lt;?</xsl:text>
          <xsl:value-of select="name($nodes[1])"/>
          <xsl:text> </xsl:text>
          <xsl:value-of select="$nodes[1]"/>
          <xsl:text disable-output-escaping='yes'>?&gt;</xsl:text>
        </xsl:when>

        <xsl:when test="not($nodes[1]/parent::*)"/> <!-- root node -->
        <xsl:when test="count($nodes[1] | $nodes[1]/../namespace::*) = count($nodes[1]/../namespace::*)"/> <!-- namespace node -->
        <xsl:when test="count($nodes[1] | $nodes[1]/../@*) = count($nodes[1]/../@*)"/> <!-- attribute node -->
      </xsl:choose>

      <xsl:call-template name="markup:as-xml">
        <xsl:with-param name="nodes" select="$nodes[position() &gt; 1]"/>
      </xsl:call-template>
    </xsl:if>
  </xsl:template>

</xsl:stylesheet>
