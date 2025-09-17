<?xml version="1.0"?>

<xsl:stylesheet
  version="1.0"
  extension-element-prefixes="doc"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
  xmlns:str="http://xsltsl.org/string"
  xmlns:cmp="http://xsltsl.org/cmp"
  exclude-result-prefixes="cmp str doc"
>

  <doc:reference xmlns="">
    <referenceinfo>
      <releaseinfo role="meta">
        $Id: cmp.xsl 6297 2006-09-14 01:32:27Z xmldoc $
      </releaseinfo>
      <author>
        <surname>Hummel</surname>
        <firstname>Mark</firstname>
      </author>
      <copyright>
        <year>2003</year>
        <holder>Mark Hummel</holder>
      </copyright>
    </referenceinfo>

    <title>XML Compare</title>

    <partintro>
      <section>
        <title>Introduction</title>

        <para>This module provides a template for comparing two xml documents. </para>

      </section>
    </partintro>

  </doc:reference>


  <doc:template name="cmp:diff">
    <refpurpose>Find differences</refpurpose>

    <refdescription>
      <para>Compare two xml documents and display differences. Two xml documents are defined to be the same if: They have the matching elements and attributes, and that the data in the elements also match. The comparison is order sensitive. </para>

      <para>The element names from the documents at the current depth are compared, followed by their values, then any attribute names and values are compared. The process is applied then to the subtrees of the documents.</para>

      <para>Notes: If there are leaf nodes in one nodeset which don't exist in the other, the value of those 'extra' elements won't appear as a difference.
      </para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>ns1</term>
          <term>ns2</term>
          <listitem>
            <para>The two nodesets which are to be compared. </para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the difference between the documents. </para>

      <para>The format of the output is an xml document. A node is added to the result tree for every difference. The node contains the type of difference (e.g element name difference, attribute value difference, etc), the value in the first nodeset and the value in the second nodeset, and the parent node. The indentation level is the depth at which the difference was found relative to the first document. </para>

    </refreturn>
  </doc:template>

  <!-- pass in a nodeset and compare. Is order sensitive. Output attribute, element and textual differences. -->

  <xsl:template name="cmp:diff">
    <xsl:param name="ns1"/>
    <xsl:param name="ns2"/>

    <!-- attribute compare -->
	<!-- Optimisation attempt 

	Can probaby change this into one loop ie -
	<xsl:for-each 
	  i = position
	 if node1[i] = node2[i]...

	  -->

	<!-- Need to check if there are two sets of attributes -->
	<xsl:choose>
	  <xsl:when test='count($ns1/attribute::*) = count($ns2/attribute::*)'>
	    <xsl:for-each select="$ns1/attribute::*">
	      <xsl:variable name="name1" select="name()"/>
	      <xsl:variable name="value1" select="."/>
	      <xsl:variable name="i" select="position()"/>
	      
	      <xsl:for-each select="$ns2/attribute::*">
		
		<xsl:variable name="j" select="position()"/>
		<xsl:variable name="name2" select="name()"/>
		<xsl:variable name="value2" select="."/>
		
		<xsl:if test="$i = $j">
		  <xsl:if test="$name1 != $name2">
		    <attributeNameDifference>
	              <parentElement><xsl:value-of select="name(..)"/></parentElement>
                      <before><xsl:value-of select="$name1"/></before>
	              <after><xsl:value-of select="$name2"/></after>
		    </attributeNameDifference>
		  </xsl:if>
		  
		  <xsl:if test="$name1 = $name2 and $value1 != $value2">
		    <attributeValueDifference>
		      <parentElement><xsl:value-of select="name(..)"/></parentElement>
		      <before><xsl:value-of select="$value1"/></before>
		      <after><xsl:value-of select="$value2"/></after>
		    </attributeValueDifference>
		  </xsl:if>
	      
		</xsl:if>
	      </xsl:for-each>
	    </xsl:for-each>
	    </xsl:when>
	  <xsl:otherwise>
	    <attributeNameDifference>
	      <parentElement>
		<xsl:value-of select="name(..)"/>
	      </parentElement>
	      <before><xsl:value-of select='$ns1/attribute::*'/></before>
	      <after><xsl:value-of select='$ns2/attribute::*'/></after>
	    </attributeNameDifference>
	  </xsl:otherwise>
	</xsl:choose>
	

  <!-- Find element differences by comparing the element names from the same position in both documents. Iterate over all the nodes in the nodeset with the largest number of elements, so the extra elements will appear as differences. -->

    <xsl:choose>
      <!-- Define loop direction based on which tree has more nodes
	   FIXME: Replacing this with one for-each and a test for the case 
	          of the second tree having more nodes would be more elegant 

	   Solution: Add variable for direction and assign the 'larger' nodeset to that
	             variable. Then do one for-each. 
	   
	   FIXME: The solution is a bit too iterative. Make it more functional

      -->
     <xsl:when test="count($ns1) &gt; count($ns2)">
       <xsl:for-each select="$ns1">
          <xsl:variable name="i" select="position()"/> 
	  
	  <xsl:message>node[<xsl:value-of select='$i'/>]:
	    <xsl:value-of select='$ns1[$i]'/>
	  </xsl:message>

	<!-- Element name compare -->
	  <xsl:if test="name($ns1[$i]) != name($ns2[$i])">
        	<elementNameDifference>
		  <parentElement><xsl:value-of select="name(..)"/></parentElement>
		  <before><xsl:value-of select="name($ns1[$i])"/></before>
		  <after><xsl:value-of select="name($ns2[$i])"/></after>
  	        </elementNameDifference>
	  </xsl:if>
	
	  <!-- Element Value compare -->
	
	  <xsl:if test="count($ns1/*) = 0">
            <xsl:if test="$ns1[$i] != $ns2[$i]">
	         <elementValueDifference>
	           <parentElement><xsl:value-of select="name(..)"/></parentElement>
		   <before><xsl:value-of select="$ns1[$i]"/></before>
 		   <after><xsl:value-of select="$ns2[$i]"/></after>
	         </elementValueDifference>
	    </xsl:if>
	 </xsl:if>
	
       </xsl:for-each>
      </xsl:when>
      <xsl:otherwise>
        <xsl:for-each select="$ns2">
          <xsl:variable name="i" select="position()"/> 

	  <!-- Element Name compare -->
	
	  <xsl:if test="name($ns1[$i]) != name($ns2[$i])">
	       <elementNameDifference>
		  <parentElement><xsl:value-of select="name(..)"/></parentElement>
		  <before><xsl:value-of select="name($ns1[$i])"/></before>
		  <after><xsl:value-of select="name($ns2[$i])"/></after>
	       </elementNameDifference>

	  </xsl:if>
	
	  <!-- value compare -->
	
	  <xsl:if test="count($ns2/*) = 0">
	     <xsl:if test="$ns2[$i] != $ns1[$i]">
		 <elementValueDifference>
		   <parentElement><xsl:value-of select="name(..)"/></parentElement>
		   <after><xsl:value-of select="$ns2[$i]"/></after>
                   <before><xsl:value-of select="$ns1[$i]"/></before>
		 </elementValueDifference>
	     </xsl:if>
	  </xsl:if>
	
        </xsl:for-each>
      </xsl:otherwise>
    </xsl:choose>
	
  <!-- stop processing when leaf node is reached. -->

    <xsl:if test="count($ns1/*) &gt; 0 and count($ns2/*) &gt; 0">
      <xsl:call-template name="cmp:diff">
            <xsl:with-param name="ns1" select="$ns1/*"/>
            <xsl:with-param name="ns2" select="$ns2/*"/>
        </xsl:call-template>
    </xsl:if>

  </xsl:template>

  <!-- Return false if the two nodesets are not identical
  -->

  <doc:template name="cmp:cmp">
    <refpurpose>Compare</refpurpose>

    <refdescription>
      <para>Recursively compare two xml nodesets, stop when a difference is found and return false. Otherwise return true if the document is identical. </para>

      <para>The element names from the documents at the current depth are compared, followed by their values, then any attribute names and values are compared. The process is applied then to the subtrees of the documents.</para>

      <para>Notes: If there are leaf nodes in one nodeset which don't exist in the other, the value of those 'extra' elements won't appear as a difference.
      </para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>ns1</term>
          <term>ns2</term>
          <listitem>
            <para>The two nodesets which are to be compared. </para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>False when the nodesets are not identical, empty otherwise. </para>

    </refreturn>
  </doc:template>

  <xsl:template name="cmp:cmp">
   <xsl:param name="ns1"/>     	
   <xsl:param name="ns2"/>     	
   <xsl:param name="depth"/>

   <xsl:choose>
     <xsl:when test='count($ns1) != count($ns2)'>
       <xsl:value-of select='"countDiff"'/>
     </xsl:when>
     <xsl:when test='count($ns1/attribute::*) != count($ns2/attribute::*)'>
       <xsl:value-of select='"countDiff"'/>
     </xsl:when>
     <xsl:when test='$ns1 and $ns2'>

       <xsl:variable name='result'>
	<xsl:call-template name='cmp:cmp'>
	  <xsl:with-param name='ns1' select='$ns1/*'/>
	  <xsl:with-param name='ns2' select='$ns2/*'/>
	  <xsl:with-param name='depth' select='$depth+1'/>
	</xsl:call-template>
	</xsl:variable>	

       <xsl:choose>
	  <xsl:when test='$result = "countDiff"'>
	    <xsl:value-of select='$result'/>
	  </xsl:when>
	  <xsl:when test='$result = "textDiff"'>
	    <xsl:value-of select='$result'/>
	  </xsl:when>	  
	  <xsl:when test='$result = ""'>

	    <xsl:variable name='keyText1' select='name($ns1)'/>
	    <xsl:variable name='keyText2' select='name($ns2)'/>
	    	    
	    <xsl:choose>
	      <!-- Check if the text of the nodesets are the same and the attributes-->
	      <xsl:when test='$ns1 = $ns2 and $keyText1 = $keyText2'>

		<!-- Check the attribute names are the same -->
		<!-- Number of attributes being different is caught higher up -->
		<xsl:if test='count($ns1/attribute::*)'>
		  <xsl:for-each select='$ns1/attribute::*'>
		    <xsl:variable name='i' select='position()'/>
		    <xsl:variable name='name1' select='name(.)'/>
                    <xsl:variable name='value1' select='.'/>
		    
		    <xsl:for-each select='$ns2/attribute::*'>
		      <xsl:variable name='j' select='position()'/>
		      <xsl:variable name='name2' select='name(.)'/>
                      <xsl:variable name='value2' select='.'/>

                      <xsl:if test='$i = $j and ($name1 != $name2 or 
                                    $value1 != $value2)'>
			<xsl:value-of select='"textDiff"'/>
		      </xsl:if>
		      
		    </xsl:for-each>
		  </xsl:for-each>
		</xsl:if>
		<!--
		<xsl:variable name='diffResult'>
		  <xsl:call-template name='cmp:diff'>
		    <xsl:with-param name='ns1' select='$ns1'/>
		    <xsl:with-param name='ns2' select='$ns2'/>
		  </xsl:call-template>
		</xsl:variable>
		
		<xsl:if test='not($diffResult = "")'>
		  <xsl:value-of select='"textDiff"'/>
		</xsl:if>
		-->

	      </xsl:when>
	      <xsl:otherwise>
		<xsl:value-of select='"textDiff"'/>
	      </xsl:otherwise>
	    </xsl:choose>
	  </xsl:when>
	</xsl:choose>
	  
     </xsl:when>
     <xsl:when test='$ns1 and not($ns2)'>
       <xsl:value-of select='"structDiff"'/>
     </xsl:when>
     <xsl:when test='$ns2 and not($ns1)'>
       <xsl:value-of select='"structDiff"'/>
     </xsl:when>
   </xsl:choose>

  </xsl:template>

</xsl:stylesheet>

