<?xml version="1.0"?>
<!DOCTYPE xsl:stylesheet [
  <!ENTITY version "1.2.1">
]>

<xsl:stylesheet
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
  exclude-result-prefixes="doc"
  version="1.0">

  <xsl:import href="string.xsl"/>
  <xsl:import href="date-time.xsl"/>
  <xsl:import href="node.xsl"/>
  <xsl:import href="uri.xsl"/>
  <xsl:import href="markup.xsl"/>
  <xsl:import href="math.xsl"/>
  <xsl:import href="cmp.xsl"/>

  <xsl:import href="svg.xsl"/>
<!--
  <xsl:import href="html/html.xsl"/>
  <xsl:import href="fo/fo.xsl"/>
-->

  <!-- For a new module, add an import element here -->
  <xsl:import href="example.xsl"/>

  <doc:book xmlns="">
    <bookinfo>
      <title>XSLT Standard Library</title>
      <subtitle>Version &version;</subtitle>
      <!-- $Id: stdlib.xsl 3991 2004-11-10 06:51:55Z balls $ -->

      <author>
        <surname>Ball</surname>
        <firstname>Steve</firstname>
      </author>
      <copyright>
        <year>2004</year>
        <year>2002</year>
        <holder>Steve Ball</holder>
      </copyright>
    </bookinfo>

    <preface>
      <para>The <ulink url="http://www.w3.org/Style/XSL">XSLT</ulink> Standard Library, <acronym>xsltsl</acronym>, provides the XSLT developer with a set of XSLT templates for commonly used functions.  These are implemented purely in XSLT, that is they do not use any extensions.</para>
      <para><acronym>xsltsl</acronym> is a <ulink url="http://sourceforge.net/projects/xsltsl/">SourceForge project</ulink>.</para>
      <para><ulink url="http://sourceforge.net/"><inlinemediaobject>
	  <imageobject>
	    <imagedata fileref="sflogo.gif" width="88" height="31"/>
	  </imageobject>
	  <textobject>
	    <phrase>SourceForge Logo</phrase>
	  </textobject>
	</inlinemediaobject></ulink></para>
      <para>Goals of the <acronym>xsltsl</acronym> project include:</para>
      <itemizedlist>
        <listitem>
          <para>Provision of a high-quality library of XSLT templates, suitable for inclusion by vendors in XSLT processor software products.</para>
        </listitem>
        <listitem>
          <para>Demonstration of best practice in XSLT stylesheet development and documentation.</para>
        </listitem>
        <listitem>
          <para>Provide examples of various techniques used to develop XSLT stylesheets (ie. a working FAQ).</para>
        </listitem>
      </itemizedlist>
    </preface>

    <chapter>
      <title>Using The Library</title>

      <para>There are two ways of using the library:</para>
      <itemizedlist>
	<listitem>
	  <para>Use a local copy of the library.</para>
	  <orderedlist>
	    <listitem>
	      <para>Download the distribution (see below).</para>
	    </listitem>
	    <listitem>
	      <para>Unpack the distribution, using either gunzip/tar or unzip.</para>
	    </listitem>
	    <listitem>
	      <para>In your stylesheet import or include either the main stylesheet, <filename>stdlib.xsl</filename>, or the stylesheet module you wish to use, such as <filename>string.xsl</filename>.  This example assumes that the distribution has been extracted into the same directory as your own stylesheet:</para>
	      <informalexample>
		<programlisting><![CDATA[
<xsl:import href="stdlib.xsl"/>
]]></programlisting>
	      </informalexample>
	    </listitem>
	  </orderedlist>
	</listitem>
	<listitem>
          <para>Import or include either the main stylesheet, or the stylesheet module you wish to use, directly from the library website; http://xsltsl.sourceforge.net/modules/.  The <filename>modules</filename> directory always contains the latest stable release.  For example:</para>
	  <informalexample>
	    <programlisting><![CDATA[
<xsl:import href="http://xsltsl.sourceforge.net/modules/stdlib.xsl"/>
]]></programlisting>
	  </informalexample>
          <para>Older versions of the library are available in subdirectories.  For example, to access version 1.1 of the library use:</para>
	  <informalexample>
	    <programlisting><![CDATA[
<xsl:import href="http://xsltsl.sourceforge.net/modules/1.1/stdlib.xsl"/>
]]></programlisting>
	  </informalexample>
	</listitem>
      </itemizedlist>
      <para>Next, add XML Namespace declarations for the modules you wish to use.  For example, to use templates from the string module, your stylesheet should have the following declaration:</para>
      <informalexample>
	<programlisting><![CDATA[
<xsl:stylesheet version="1.0"
	xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
	xmlns:str="http://xsltsl.org/string">

<xsl:import href="http://xsltsl.sourceforge.net/modules/stdlib.xsl"/>
]]></programlisting>
      </informalexample>
      <para>Finally, use a template with the <sgmltag>call-template</sgmltag> element.  Most templates require parameters, which are passed using the <sgmltag>with-param</sgmltag> element.  For example:</para>
      <informalexample>
	<programlisting><![CDATA[
<xsl:template match="foo">
  <xsl:call-template name="str:subst">
    <xsl:with-param name="text" select="."/>
    <xsl:with-param name="replace">a word</xsl:with-param>
    <xsl:with-param name="with">another word</xsl:with-param>
  </xsl:call-template>
</xsl:template>
]]></programlisting>
      </informalexample>
    </chapter>

    <chapter>
      <title>Obtaining The Library</title>

      <para>The XSLT Standard Library is available for download as either:</para>
      <itemizedlist>
	<listitem>
	  <para>Gzip'd tarball: <ulink url="http://prdownloads.sourceforge.net/xsltsl/xsltsl-&version;.tar.gz">http://prdownloads.sourceforge.net/xsltsl/xsltsl-&version;.tar.gz</ulink></para>
	</listitem>
	<listitem>
	  <para>Zip file: <ulink url="http://prdownloads.sourceforge.net/xsltsl/xsltsl-&version;.zip">http://prdownloads.sourceforge.net/xsltsl/xsltsl-&version;.zip</ulink></para>
	</listitem>
      </itemizedlist>
    </chapter>

    <chapter>
      <title>Getting Involved</title>

      <para>Contributions to the project are most welcome, and may be in the form of stylesheet modules, patches, bug reports or sample code.  Any contributed code must use the LGPL license to be accepted into the library.</para>

      <para>See the SourceForge Project Page <ulink url="http://sourceforge.net/projects/xsltsl/">http://sourceforge.net/projects/xsltsl/</ulink> for information on the development of the project.  Bug reports may be submitted here.</para>

      <para>See the project Web Page <ulink url="http://xsltsl.sourceforge.net/">http://xsltsl.sourceforge.net/</ulink> for documentation.</para>

      <para>There are three mailing lists for the project:</para>
      <variablelist>
	<varlistentry>
	  <term><email>xsltsl-users@lists.sourceforge.net</email></term>
	  <listitem>
	    <para>Discussion of the use of <acronym>xsltsl</acronym>.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term><email>xsltsl-devel@lists.sourceforge.net</email></term>
	  <listitem>
	    <para>Discussion of the development of <acronym>xsltsl</acronym>.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term><email>xsltsl-announce@lists.sourceforge.net</email></term>
	  <listitem>
	    <para>Project announcements.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </chapter>

    <chapter>
      <title>XML Namespaces</title>

      <para>Apart from the XSLT XML Namespace (http://www.w3.org/1999/XSL/Transform), <acronym>xsltsl</acronym> employs a number of XML Namespaces to allow inclusion of the library in developer stylesheets.  In addition, documentation is defined in a separate namespace.</para>
      <para>Each module is allocated a namespace URI by appending the module name to the URI for the project, http://xsltsl.org/.  For example, the string module has the namespace URI http://xsltsl.org/string.</para>
      <para>All documentation is written using an <ulink url="docbook-extensions.html">extension</ulink> of <ulink url="http://www.docbook.org/">DocBook</ulink> designed for <ulink url="docbook-extensions.html">embedding DocBook into XSLT stylesheets</ulink>.  The namespace URI for DocBook embedded in stylesheets is http://xsltsl.org/xsl/documentation/1.0</para>
    </chapter>

    <chapter>
      <title>Engineering Standards</title>

      <para>In order to maintain a high engineering standard, all modules and contributions to the <acronym>xsltsl</acronym> project must adhere to the following coding and documentation standards.  Submissions which do not meet (or exceed) this standard will not be accepted.</para>
      <itemizedlist>
        <listitem>
          <para>All stylesheets must be indented, with each level indented by two spaces.  NB. a simple stylesheet could be used to enforce/fix this.</para>
        </listitem>
        <listitem>
          <para>Templates are named using a qualified name (QName).  The namespace URI for the template's containing stylesheet is assigned as above.</para>
        </listitem>
        <listitem>
          <para>Parameters for templates should use sensible names.  Where possible (or if in doubt), follow these conventions:</para>
          <itemizedlist>
            <listitem>
              <para>A parameter containing a single node is named <parametername>node</parametername>.  Where more than one parameter contains a single node, the suffix <parametername>Node</parametername> is appended to the parameter name, eg. <parametername>referenceNode</parametername></para>
            </listitem>
            <listitem>
              <para>A parameter which potentially contains multiple nodes is named <parametername>nodes</parametername>.  Where more than one parameter potentially contains multiple nodes, the suffix <parametername>Nodes</parametername> is appended to the parameter name, eg. <parametername>copyNodes</parametername></para>
            </listitem>
            <listitem>
              <para>A parameter which contains a string value is named <parametername>text</parametername>.</para>
            </listitem>
          </itemizedlist>
        </listitem>
        <listitem>
          <para>All templates in each stylesheet must be documented.  A template is documented as a <ulink url="http://www.docbook.org/">DocBook</ulink> RefEntry.</para>
        </listitem>
        <listitem>
          <para>Every stylesheet must include a test suite.  The test system is in the <filename>test</filename> subdirectory.  See <ulink url="test/test.html">test/test.html</ulink> for further details.</para>
        </listitem>
      </itemizedlist>

      <para>An <ulink url="example.xsl">example stylesheet</ulink> has been provided, which acts as a template for new stylesheet modules.</para>

    </chapter>

    <chapter>
      <title>Related Work</title>

      <para>The <ulink url="http://www.exslt.org/">EXSLT</ulink> project is creating a library to standardise extension functions.  The XSLT Standard Library is complementary to the EXSLT project.</para>

    </chapter>

    <chapter>
      <title>Reference Documentation</title>

      <para>Reference documentation is available for each module.</para>

      <section>
        <title>String Processing</title>

        <itemizedlist>
          <listitem>
            <para><ulink url="string.html">string.xsl</ulink></para>
          </listitem>
        </itemizedlist>
      </section>

      <section>
        <title>Nodes</title>

        <itemizedlist>
          <listitem>
            <para><ulink url="node.html">node.xsl</ulink></para>
          </listitem>
        </itemizedlist>
      </section>

      <section>
        <title>Date/Time Processing</title>

        <itemizedlist>
          <listitem>
            <para><ulink url="date-time.html">date-time.xsl</ulink></para>
          </listitem>
        </itemizedlist>
      </section>

      <section>
        <title>Mathematics</title>

        <itemizedlist>
          <listitem>
            <para><ulink url="math.html">math.xsl</ulink></para>
          </listitem>
        </itemizedlist>
      </section>

      <section>
        <title>URI (Uniform Resource Identifier) Processing</title>

        <itemizedlist>
          <listitem>
            <para><ulink url="uri.html">uri.xsl</ulink></para>
          </listitem>
        </itemizedlist>
      </section>

      <section>
        <title>Comparing Nodesets</title>

        <itemizedlist>
          <listitem>
            <para><ulink url="cmp.html">cmp.xsl</ulink></para>
          </listitem>
        </itemizedlist>
      </section>

      <section>
        <title>Generating XML Markup</title>

        <itemizedlist>
          <listitem>
            <para><ulink url="markup.html">markup.xsl</ulink></para>
          </listitem>
        </itemizedlist>
      </section>

      <section>
        <title>Presentation Media Support</title>

        <itemizedlist>
          <listitem>
            <para>Scalable Vector Graphics: <ulink url="svg.html">svg.xsl</ulink></para>
          </listitem>
<!--
          <listitem>
            <para><ulink url="html/html.html">html/html.xsl</ulink></para>
          </listitem>
          <listitem>
            <para><ulink url="fo/fo.html">fo/fo.xsl</ulink></para>
          </listitem>
-->
        </itemizedlist>
      </section>

      <section>
        <title>Example</title>

        <!-- Add a new module in a similar fashion -->

        <itemizedlist>
          <listitem>
            <para><ulink url="example.html">example.xsl</ulink></para>
          </listitem>
        </itemizedlist>
      </section>
    </chapter>

  </doc:book>

</xsl:stylesheet>
