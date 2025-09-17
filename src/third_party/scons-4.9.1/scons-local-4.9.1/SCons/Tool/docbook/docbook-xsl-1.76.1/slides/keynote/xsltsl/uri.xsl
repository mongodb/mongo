<?xml version="1.0"?>

<xsl:stylesheet
  version="1.0"
  extension-element-prefixes="doc"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
  xmlns:uri="http://xsltsl.org/uri"
>

  <doc:reference xmlns="">
    <referenceinfo>
      <releaseinfo role="meta">
        $Id: uri.xsl 3991 2004-11-10 06:51:55Z balls $
      </releaseinfo>
      <author>
        <surname>Diamond</surname>
        <firstname>Jason</firstname>
      </author>
      <copyright>
        <year>2001</year>
        <holder>Jason Diamond</holder>
      </copyright>
    </referenceinfo>

    <title>URI (Uniform Resource Identifier) Processing</title>

    <partintro>
      <section>
        <title>Introduction</title>
        <para>This module provides templates for processing URIs (Uniform Resource Identifers).</para>
      </section>
    </partintro>

  </doc:reference>

  <doc:template name="uri:is-absolute-uri" xmlns="">
    <refpurpose>Determines if a URI is absolute or relative.</refpurpose>

    <refdescription>
      <para>Absolute URIs start with a scheme (like "http:" or "mailto:").</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>uri</term>
          <listitem>
            <para>An absolute or relative URI.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns 'true' if the URI is absolute or '' if it's not.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="uri:is-absolute-uri">
    <xsl:param name="uri"/>

    <xsl:if test="contains($uri, ':')">
      <xsl:value-of select="true()"/>
    </xsl:if>

  </xsl:template>

  <doc:template name="uri:get-uri-scheme" xmlns="">
    <refpurpose>Gets the scheme part of a URI.</refpurpose>

    <refdescription>
      <para>The ':' is not part of the scheme.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>uri</term>
          <listitem>
            <para>An absolute or relative URI.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the scheme (without the ':') or '' if the URI is relative.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="uri:get-uri-scheme">
    <xsl:param name="uri"/>
    <xsl:if test="contains($uri, ':')">
      <xsl:value-of select="substring-before($uri, ':')"/>
    </xsl:if>
  </xsl:template>

  <doc:template name="uri:get-uri-authority" xmlns="">
    <refpurpose>Gets the authority part of a URI.</refpurpose>

    <refdescription>
      <para>The authority usually specifies the host machine for a resource. It always follows '//' in a typical URI.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>uri</term>
          <listitem>
            <para>An absolute or relative URI.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the authority (without the '//') or '' if the URI has no authority.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="uri:get-uri-authority">
    <xsl:param name="uri"/>

    <xsl:variable name="a">
      <xsl:choose>
        <xsl:when test="contains($uri, ':')">
          <xsl:if test="substring(substring-after($uri, ':'), 1, 2) = '//'">
              <xsl:value-of select="substring(substring-after($uri, ':'), 3)"/>
          </xsl:if>
        </xsl:when>
        <xsl:otherwise>
          <xsl:if test="substring($uri, 1, 2) = '//'">
            <xsl:value-of select="substring($uri, 3)"/>
          </xsl:if>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:variable>

    <xsl:choose>
      <xsl:when test="contains($a, '/')">
        <xsl:value-of select="substring-before($a, '/')" />
      </xsl:when>
      <xsl:when test="contains($a, '?')">
        <xsl:value-of select="substring-before($a, '?')" />
      </xsl:when>
      <xsl:when test="contains($a, '#')">
        <xsl:value-of select="substring-before($a, '#')" />
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$a" />
      </xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <doc:template name="uri:get-uri-path" xmlns="">
    <refpurpose>Gets the path part of a URI.</refpurpose>

    <refdescription>
      <para>The path usually comes after the '/' in a URI.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>uri</term>
          <listitem>
            <para>An absolute or relative URI.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the path (with any leading '/') or '' if the URI has no path.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="uri:get-uri-path">
    <xsl:param name="uri"/>

    <xsl:variable name="p">
      <xsl:choose>
        <xsl:when test="contains($uri, '//')">
          <xsl:if test="contains(substring-after($uri, '//'), '/')">
            <xsl:value-of select="concat('/', substring-after(substring-after($uri, '//'), '/'))"/>
          </xsl:if>
        </xsl:when>
        <xsl:otherwise>
          <xsl:choose>
            <xsl:when test="contains($uri, ':')">
              <xsl:value-of select="substring-after($uri, ':')"/>
            </xsl:when>
            <xsl:otherwise>
              <xsl:value-of select="$uri"/>
            </xsl:otherwise>
          </xsl:choose>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:variable>

    <xsl:choose>
      <xsl:when test="contains($p, '?')">
        <xsl:value-of select="substring-before($p, '?')" />
      </xsl:when>
      <xsl:when test="contains($p, '#')">
        <xsl:value-of select="substring-before($p, '#')" />
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$p" />
      </xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <doc:template name="uri:get-uri-query" xmlns="">
    <refpurpose>Gets the query part of a URI.</refpurpose>

    <refdescription>
      <para>The query comes after the '?' in a URI.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>uri</term>
          <listitem>
            <para>An absolute or relative URI.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the query (without the '?') or '' if the URI has no query.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="uri:get-uri-query">
    <xsl:param name="uri"/>

    <xsl:variable name="q" select="substring-after($uri, '?')"/>

    <xsl:choose>
      <xsl:when test="contains($q, '#')">
        <xsl:value-of select="substring-before($q, '#')"/>
      </xsl:when>
      <xsl:otherwise><xsl:value-of select="$q"/></xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <doc:template name="uri:get-uri-fragment" xmlns="">
    <refpurpose>Gets the fragment part of a URI.</refpurpose>

    <refdescription>
      <para>The fragment comes after the '#' in a URI.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>uri</term>
          <listitem>
            <para>An absolute or relative URI.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the fragment (without the '#') or '' if the URI has no fragment.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="uri:get-uri-fragment">
    <xsl:param name="uri"/>

    <xsl:value-of select="substring-after($uri, '#')"/>

  </xsl:template>

  <doc:template name="uri:resolve-uri" xmlns="">
    <refpurpose>Resolves a URI reference against a base URI.</refpurpose>

    <refdescription>
      <para>This template follows the guidelines specified by <ulink url="ftp://ftp.isi.edu/in-notes/rfc2396.txt">RFC 2396</ulink>.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>reference</term>
          <listitem>
            <para>A (potentially relative) URI reference.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>base</term>
          <listitem>
            <para>The base URI.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>document</term>
          <listitem>
            <para>The URI of the current document. This defaults to the value of the base URI if not specified.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>The "combined" URI.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="uri:resolve-uri">
    <xsl:param name="reference"/>
    <xsl:param name="base"/>
    <xsl:param name="document" select="$base"/>

    <xsl:variable name="reference-scheme">
      <xsl:call-template name="uri:get-uri-scheme">
        <xsl:with-param name="uri" select="$reference"/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:variable name="reference-authority">
      <xsl:call-template name="uri:get-uri-authority">
        <xsl:with-param name="uri" select="$reference"/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:variable name="reference-path">
      <xsl:call-template name="uri:get-uri-path">
        <xsl:with-param name="uri" select="$reference"/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:variable name="reference-query">
      <xsl:call-template name="uri:get-uri-query">
        <xsl:with-param name="uri" select="$reference"/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:variable name="reference-fragment">
      <xsl:call-template name="uri:get-uri-fragment">
        <xsl:with-param name="uri" select="$reference"/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:choose>

      <xsl:when test="
        not(string-length($reference-scheme)) and
        not(string-length($reference-authority)) and
        not(string-length($reference-path)) and
        not(string-length($reference-query))"
      >

        <xsl:choose>
          <xsl:when test="contains($document, '?')">
            <xsl:value-of select="substring-before($document, '?')"/>
          </xsl:when>
          <xsl:when test="contains($document, '#')">
            <xsl:value-of select="substring-before($document, '#')"/>
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select="$document"/>
          </xsl:otherwise>
        </xsl:choose>

        <xsl:if test="string-length($reference-fragment)">
          <xsl:value-of select="concat('#', $reference-fragment)"/>
        </xsl:if>

      </xsl:when>

      <xsl:when test="string-length($reference-scheme)">

        <xsl:value-of select="$reference"/>

      </xsl:when>

      <xsl:otherwise>

        <xsl:variable name="base-scheme">
          <xsl:call-template name="uri:get-uri-scheme">
            <xsl:with-param name="uri" select="$base"/>
          </xsl:call-template>
        </xsl:variable>

        <xsl:variable name="base-authority">
          <xsl:call-template name="uri:get-uri-authority">
            <xsl:with-param name="uri" select="$base"/>
          </xsl:call-template>
        </xsl:variable>

        <xsl:variable name="base-path">
          <xsl:call-template name="uri:get-uri-path">
            <xsl:with-param name="uri" select="$base"/>
          </xsl:call-template>
        </xsl:variable>

        <xsl:variable name="base-query">
          <xsl:call-template name="uri:get-uri-query">
            <xsl:with-param name="uri" select="$base"/>
          </xsl:call-template>
        </xsl:variable>

        <xsl:variable name="base-fragment">
          <xsl:call-template name="uri:get-uri-fragment">
            <xsl:with-param name="uri" select="$base"/>
          </xsl:call-template>
        </xsl:variable>

        <xsl:variable name="result-authority">
          <xsl:choose>
            <xsl:when test="string-length($reference-authority)">
              <xsl:value-of select="$reference-authority"/>
            </xsl:when>
            <xsl:otherwise>
              <xsl:value-of select="$base-authority"/>
            </xsl:otherwise>
          </xsl:choose>
        </xsl:variable>

        <xsl:variable name="result-path">
          <xsl:choose>
            <!-- don't normalize absolute paths -->
            <xsl:when test="starts-with($reference-path, '/')">
              <xsl:value-of select="$reference-path" />
            </xsl:when>
            <xsl:otherwise>
              <xsl:call-template name="uri:normalize-path">
                <xsl:with-param name="path">
                  <xsl:if test="string-length($reference-authority) = 0 and substring($reference-path, 1, 1) != '/'">
                    <xsl:call-template name="uri:get-path-without-file">
                      <xsl:with-param name="path-with-file" select="$base-path"/>
                    </xsl:call-template>
                    <xsl:value-of select="'/'"/>
                  </xsl:if>
                  <xsl:value-of select="$reference-path"/>
                </xsl:with-param>
              </xsl:call-template>
            </xsl:otherwise>
          </xsl:choose>
        </xsl:variable>

        <xsl:value-of select="concat($base-scheme, '://', $result-authority, $result-path)"/>

        <xsl:if test="string-length($reference-query)">
          <xsl:value-of select="concat('?', $reference-query)"/>
        </xsl:if>

        <xsl:if test="string-length($reference-fragment)">
          <xsl:value-of select="concat('#', $reference-fragment)"/>
        </xsl:if>

      </xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <xsl:template name="uri:get-path-without-file">
    <xsl:param name="path-with-file" />
    <xsl:param name="path-without-file" />

    <xsl:choose>
      <xsl:when test="contains($path-with-file, '/')">
        <xsl:call-template name="uri:get-path-without-file">
          <xsl:with-param name="path-with-file" select="substring-after($path-with-file, '/')" />
          <xsl:with-param name="path-without-file">
            <xsl:choose>
              <xsl:when test="$path-without-file">
                <xsl:value-of select="concat($path-without-file, '/', substring-before($path-with-file, '/'))" />
              </xsl:when>
              <xsl:otherwise>
                <xsl:value-of select="substring-before($path-with-file, '/')" />
              </xsl:otherwise>
            </xsl:choose>
          </xsl:with-param>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$path-without-file" />
      </xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <xsl:template name="uri:normalize-path">
    <xsl:param name="path"/>
    <xsl:param name="result" select="''"/>

    <xsl:choose>
      <xsl:when test="string-length($path)">
        <xsl:choose>
          <xsl:when test="$path = '/'">
            <xsl:value-of select="concat($result, '/')"/>
          </xsl:when>
          <xsl:when test="$path = '.'">
            <xsl:value-of select="concat($result, '/')"/>
          </xsl:when>
          <xsl:when test="$path = '..'">
            <xsl:call-template name="uri:get-path-without-file">
              <xsl:with-param name="path-with-file" select="$result"/>
            </xsl:call-template>
            <xsl:value-of select="'/'"/>
          </xsl:when>
          <xsl:when test="contains($path, '/')">
            <!-- the current segment -->
            <xsl:variable name="s" select="substring-before($path, '/')"/>
            <!-- the remaining path -->
            <xsl:variable name="p">
              <xsl:choose>
                <xsl:when test="substring-after($path, '/') = ''">
                  <xsl:value-of select="'/'"/>
                </xsl:when>
                <xsl:otherwise>
                  <xsl:value-of select="substring-after($path, '/')"/>
                </xsl:otherwise>
              </xsl:choose>
            </xsl:variable>
            <xsl:choose>
              <xsl:when test="$s = ''">
                <xsl:call-template name="uri:normalize-path">
                  <xsl:with-param name="path" select="$p"/>
                  <xsl:with-param name="result" select="$result"/>
                </xsl:call-template>
              </xsl:when>
              <xsl:when test="$s = '.'">
                <xsl:call-template name="uri:normalize-path">
                  <xsl:with-param name="path" select="$p"/>
                  <xsl:with-param name="result" select="$result"/>
                </xsl:call-template>
              </xsl:when>
              <xsl:when test="$s = '..'">
                <xsl:choose>
                  <xsl:when test="string-length($result) and (substring($result, string-length($result) - 2) != '/..')">
                    <xsl:call-template name="uri:normalize-path">
                      <xsl:with-param name="path" select="$p"/>
                      <xsl:with-param name="result">
                        <xsl:call-template name="uri:get-path-without-file">
                          <xsl:with-param name="path-with-file" select="$result"/>
                        </xsl:call-template>
                      </xsl:with-param>
                    </xsl:call-template>
                  </xsl:when>
                  <xsl:otherwise>
                    <xsl:call-template name="uri:normalize-path">
                      <xsl:with-param name="path" select="$p"/>
                      <xsl:with-param name="result" select="concat($result, '/..')"/>
                    </xsl:call-template>
                  </xsl:otherwise>
                </xsl:choose>
              </xsl:when>
              <xsl:otherwise>
                <xsl:call-template name="uri:normalize-path">
                  <xsl:with-param name="path" select="$p"/>
                  <xsl:with-param name="result" select="concat($result, '/', $s)"/>
                </xsl:call-template>
              </xsl:otherwise>
            </xsl:choose>
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select="concat($result, '/', $path)"/>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$result"/>
      </xsl:otherwise>
    </xsl:choose>

  </xsl:template>

</xsl:stylesheet>
