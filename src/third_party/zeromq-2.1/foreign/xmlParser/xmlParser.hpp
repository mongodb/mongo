/****************************************************************************/
/*! \mainpage XMLParser library
 * \section intro_sec Introduction
 *
 * This is a basic XML parser written in ANSI C++ for portability.
 * It works by using recursion and a node tree for breaking
 * down the elements of an XML document.

 * Copyright (c) 2002, Frank Vanden Berghen
 * All rights reserved.
 *
 * The following license terms apply to projects that are in some way related to
 * the "ZeroMQ project", including applications
 * using "ZeroMQ project" and tools developed
 * for enhancing "ZeroMQ project". All other projects
 * (not related to "ZeroMQ project") have to use this
 * code under the Aladdin Free Public License (AFPL)
 * See the file "AFPL-license.txt" for more informations about the AFPL license.
 * (see http://www.artifex.com/downloads/doc/Public.htm for detailed AFPL terms)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Frank Vanden Berghen nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Frank Vanden Berghen ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <copyright holder> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @version     V2.39
 * @author      Frank Vanden Berghen
 *
 * \section tutorial First Tutorial
 * You can follow a simple <a href="../../xmlParser.html">Tutorial</a> to know the basics...
 *
 * \section usage General usage: How to include the XMLParser library inside your project.
 *
 * The library is composed of two files: <a href="../../xmlParser.cpp">xmlParser.cpp</a> and
 * <a href="../../xmlParser.h">xmlParser.h</a>. These are the ONLY 2 files that you need when
 * using the library inside your own projects.
 *
 * All the functions of the library are documented inside the comments of the file
 * <a href="../../xmlParser.h">xmlParser.h</a>. These comments can be transformed in
 * full-fledged HTML documentation using the DOXYGEN software: simply type: "doxygen doxy.cfg"
 *
 * By default, the XMLParser library uses (char*) for string representation.To use the (wchar_t*)
 * version of the library, you need to define the "_UNICODE" preprocessor definition variable
 * (this is usually done inside your project definition file) (This is done automatically for you
 * when using Visual Studio).
 *
 * \section example Advanced Tutorial and Many Examples of usage.
 *
 * Some very small introductory examples are described inside the Tutorial file
 * <a href="../../xmlParser.html">xmlParser.html</a>
 *
 * Some additional small examples are also inside the file <a href="../../xmlTest.cpp">xmlTest.cpp</a>
 * (for the "char*" version of the library) and inside the file
 * <a href="../../xmlTestUnicode.cpp">xmlTestUnicode.cpp</a> (for the "wchar_t*"
 * version of the library). If you have a question, please review these additionnal examples
 * before sending an e-mail to the author.
 *
 * To build the examples:
 * - linux/unix: type "make"
 * - solaris: type "make -f makefile.solaris"
 * - windows: Visual Studio: double-click on xmlParser.dsw
 *   (under Visual Studio .NET, the .dsp and .dsw files will be automatically converted to .vcproj and .sln files)
 *
 * In order to build the examples you need some additional files:
 * - linux/unix: makefile
 * - solaris: makefile.solaris
 * - windows: Visual Studio: *.dsp, xmlParser.dsw and also xmlParser.lib and xmlParser.dll
 *
 * \section debugging Debugging with the XMLParser library
 *
 * \subsection debugwin Debugging under WINDOWS
 *
 * 	Inside Visual C++, the "debug versions" of the memory allocation functions are
 * 	very slow: Do not forget to compile in "release mode" to get maximum speed.
 * 	When I have to debug a software that is using the XMLParser Library, it was usually
 * 	a nightmare because the library was sooOOOoooo slow in debug mode (because of the
 *  slow memory allocations in Debug mode). To solve this
 * 	problem, during all the debugging session, I use a very fast DLL version of the
 * 	XMLParser Library (the DLL is compiled in release mode). Using the DLL version of
 * 	the XMLParser Library allows me to have lightening XML parsing speed even in debug!
 * 	Other than that, the DLL version is useless: In the release version of my tool,
 * 	I always use the normal, ".cpp"-based, XMLParser Library (I simply include the
 * <a href="../../xmlParser.cpp">xmlParser.cpp</a> and
 * <a href="../../xmlParser.h">xmlParser.h</a> files into the project).
 *
 * 	The file <a href="../../XMLNodeAutoexp.txt">XMLNodeAutoexp.txt</a> contains some
 * "tweaks" that improve substancially the display of the content of the XMLNode objects
 * inside the Visual Studio Debugger. Believe me, once you have seen inside the debugger
 * the "smooth" display of the XMLNode objects, you cannot live without it anymore!
 *
 * \subsection debuglinux Debugging under LINUX/UNIX
 *
 * 	The speed of the debug version of the XMLParser library is tolerable so no extra
 * work.has been done.
 *
 ****************************************************************************/

#ifndef __INCLUDE_XML_NODE__
#define __INCLUDE_XML_NODE__

#include <stdlib.h>

#ifdef _UNICODE
// If you comment the next "define" line then the library will never "switch to" _UNICODE (wchar_t*) mode (16/32 bits per characters).
// This is useful when you get error messages like:
//    'XMLNode::openFileHelper' : cannot convert parameter 2 from 'const char [5]' to 'const wchar_t *'
// The _XMLWIDECHAR preprocessor variable force the XMLParser library into either utf16/32-mode (the proprocessor variable
// must be defined) or utf8-mode(the pre-processor variable must be undefined).
#define _XMLWIDECHAR
#endif

#if defined(WIN32) || defined(UNDER_CE) || defined(_WIN32) || defined(WIN64) || defined(__BORLANDC__)
// comment the next line if you are under windows and the compiler is not Microsoft Visual Studio (6.0 or .NET) or Borland
#define _XMLWINDOWS
#endif

#ifdef XMLDLLENTRY
#undef XMLDLLENTRY
#endif
#ifdef _USE_XMLPARSER_DLL
#ifdef _DLL_EXPORTS_
#define XMLDLLENTRY __declspec(dllexport)
#else
#define XMLDLLENTRY __declspec(dllimport)
#endif
#else
#define XMLDLLENTRY
#endif

// uncomment the next line if you want no support for wchar_t* (no need for the <wchar.h> or <tchar.h> libraries anymore to compile)
//#define XML_NO_WIDE_CHAR

#ifdef XML_NO_WIDE_CHAR
#undef _XMLWINDOWS
#undef _XMLWIDECHAR
#endif

#ifdef _XMLWINDOWS
#include <tchar.h>
#else
#define XMLDLLENTRY
#ifndef XML_NO_WIDE_CHAR
#include <wchar.h> // to have 'wcsrtombs' for ANSI version
                   // to have 'mbsrtowcs' for WIDECHAR version
#endif
#endif

// Some common types for char set portable code
#ifdef _XMLWIDECHAR
    #define _CXML(c) L ## c
    #define XMLCSTR const wchar_t *
    #define XMLSTR  wchar_t *
    #define XMLCHAR wchar_t
#else
    #define _CXML(c) c
    #define XMLCSTR const char *
    #define XMLSTR  char *
    #define XMLCHAR char
#endif
#ifndef FALSE
    #define FALSE 0
#endif /* FALSE */
#ifndef TRUE
    #define TRUE 1
#endif /* TRUE */


/// Enumeration for XML parse errors.
typedef enum XMLError
{
    eXMLErrorNone = 0,
    eXMLErrorMissingEndTag,
    eXMLErrorNoXMLTagFound,
    eXMLErrorEmpty,
    eXMLErrorMissingTagName,
    eXMLErrorMissingEndTagName,
    eXMLErrorUnmatchedEndTag,
    eXMLErrorUnmatchedEndClearTag,
    eXMLErrorUnexpectedToken,
    eXMLErrorNoElements,
    eXMLErrorFileNotFound,
    eXMLErrorFirstTagNotFound,
    eXMLErrorUnknownCharacterEntity,
    eXMLErrorCharacterCodeAbove255,
    eXMLErrorCharConversionError,
    eXMLErrorCannotOpenWriteFile,
    eXMLErrorCannotWriteFile,

    eXMLErrorBase64DataSizeIsNotMultipleOf4,
    eXMLErrorBase64DecodeIllegalCharacter,
    eXMLErrorBase64DecodeTruncatedData,
    eXMLErrorBase64DecodeBufferTooSmall
} XMLError;


/// Enumeration used to manage type of data. Use in conjunction with structure XMLNodeContents
typedef enum XMLElementType
{
    eNodeChild=0,
    eNodeAttribute=1,
    eNodeText=2,
    eNodeClear=3,
    eNodeNULL=4
} XMLElementType;

/// Structure used to obtain error details if the parse fails.
typedef struct XMLResults
{
    enum XMLError error;
    int  nLine,nColumn;
} XMLResults;

/// Structure for XML clear (unformatted) node (usually comments)
typedef struct XMLClear {
    XMLCSTR lpszValue; XMLCSTR lpszOpenTag; XMLCSTR lpszCloseTag;
} XMLClear;

/// Structure for XML attribute.
typedef struct XMLAttribute {
    XMLCSTR lpszName; XMLCSTR lpszValue;
} XMLAttribute;

/// XMLElementPosition are not interchangeable with simple indexes
typedef int XMLElementPosition;

struct XMLNodeContents;

/** @defgroup XMLParserGeneral The XML parser */

/// Main Class representing a XML node
/**
 * All operations are performed using this class.
 * \note The constructors of the XMLNode class are protected, so use instead one of these four methods to get your first instance of XMLNode:
 * <ul>
 *    <li> XMLNode::parseString </li>
 *    <li> XMLNode::parseFile </li>
 *    <li> XMLNode::openFileHelper </li>
 *    <li> XMLNode::createXMLTopNode (or XMLNode::createXMLTopNode_WOSD)</li>
 * </ul> */
typedef struct XMLDLLENTRY XMLNode
{
  private:

    struct XMLNodeDataTag;

    /// Constructors are protected, so use instead one of: XMLNode::parseString, XMLNode::parseFile, XMLNode::openFileHelper, XMLNode::createXMLTopNode
    XMLNode(struct XMLNodeDataTag *pParent, XMLSTR lpszName, char isDeclaration);
    /// Constructors are protected, so use instead one of: XMLNode::parseString, XMLNode::parseFile, XMLNode::openFileHelper, XMLNode::createXMLTopNode
    XMLNode(struct XMLNodeDataTag *p);

  public:
    static XMLCSTR getVersion();///< Return the XMLParser library version number

    /** @defgroup conversions Parsing XML files/strings to an XMLNode structure and Rendering XMLNode's to files/string.
     * @ingroup XMLParserGeneral
     * @{ */

    /// Parse an XML string and return the root of a XMLNode tree representing the string.
    static XMLNode parseString   (XMLCSTR  lpXMLString, XMLCSTR tag=NULL, XMLResults *pResults=NULL);
    /**< The "parseString" function parse an XML string and return the root of a XMLNode tree. The "opposite" of this function is
     * the function "createXMLString" that re-creates an XML string from an XMLNode tree. If the XML document is corrupted, the
     * "parseString" method will initialize the "pResults" variable with some information that can be used to trace the error.
     * If you still want to parse the file, you can use the APPROXIMATE_PARSING option as explained inside the note at the
     * beginning of the "xmlParser.cpp" file.
     *
     * @param lpXMLString the XML string to parse
     * @param tag  the name of the first tag inside the XML file. If the tag parameter is omitted, this function returns a node that represents the head of the xml document including the declaration term (<? ... ?>).
     * @param pResults a pointer to a XMLResults variable that will contain some information that can be used to trace the XML parsing error. You can have a user-friendly explanation of the parsing error with the "getError" function.
     */

    /// Parse an XML file and return the root of a XMLNode tree representing the file.
    static XMLNode parseFile     (XMLCSTR     filename, XMLCSTR tag=NULL, XMLResults *pResults=NULL);
    /**< The "parseFile" function parse an XML file and return the root of a XMLNode tree. The "opposite" of this function is
     * the function "writeToFile" that re-creates an XML file from an XMLNode tree. If the XML document is corrupted, the
     * "parseFile" method will initialize the "pResults" variable with some information that can be used to trace the error.
     * If you still want to parse the file, you can use the APPROXIMATE_PARSING option as explained inside the note at the
     * beginning of the "xmlParser.cpp" file.
     *
     * @param filename the path to the XML file to parse
     * @param tag the name of the first tag inside the XML file. If the tag parameter is omitted, this function returns a node that represents the head of the xml document including the declaration term (<? ... ?>).
     * @param pResults a pointer to a XMLResults variable that will contain some information that can be used to trace the XML parsing error. You can have a user-friendly explanation of the parsing error with the "getError" function.
     */

    /// Parse an XML file and return the root of a XMLNode tree representing the file. A very crude error checking is made. An attempt to guess the Char Encoding used in the file is made.
    static XMLNode openFileHelper(XMLCSTR     filename, XMLCSTR tag=NULL);
    /**< The "openFileHelper" function reports to the screen all the warnings and errors that occurred during parsing of the XML file.
     * This function also tries to guess char Encoding (UTF-8, ASCII or SHIT-JIS) based on the first 200 bytes of the file. Since each
     * application has its own way to report and deal with errors, you should rather use the "parseFile" function to parse XML files
     * and program yourself thereafter an "error reporting" tailored for your needs (instead of using the very crude "error reporting"
     * mechanism included inside the "openFileHelper" function).
     *
     * If the XML document is corrupted, the "openFileHelper" method will:
     *         - display an error message on the console (or inside a messageBox for windows).
     *         - stop execution (exit).
     *
     * I strongly suggest that you write your own "openFileHelper" method tailored to your needs. If you still want to parse
     * the file, you can use the APPROXIMATE_PARSING option as explained inside the note at the beginning of the "xmlParser.cpp" file.
     *
     * @param filename the path of the XML file to parse.
     * @param tag the name of the first tag inside the XML file. If the tag parameter is omitted, this function returns a node that represents the head of the xml document including the declaration term (<? ... ?>).
     */

    static XMLCSTR getError(XMLError error); ///< this gives you a user-friendly explanation of the parsing error

    /// Create an XML string starting from the current XMLNode.
    XMLSTR createXMLString(int nFormat=1, int *pnSize=NULL) const;
    /**< The returned string should be free'd using the "freeXMLString" function.
     *
     *   If nFormat==0, no formatting is required otherwise this returns an user friendly XML string from a given element
     *   with appropriate white spaces and carriage returns. if pnSize is given it returns the size in character of the string. */

    /// Save the content of an xmlNode inside a file
    XMLError writeToFile(XMLCSTR filename,
                         const char *encoding=NULL,
                         char nFormat=1) const;
    /**< If nFormat==0, no formatting is required otherwise this returns an user friendly XML string from a given element with appropriate white spaces and carriage returns.
     * If the global parameter "characterEncoding==encoding_UTF8", then the "encoding" parameter is ignored and always set to "utf-8".
     * If the global parameter "characterEncoding==encoding_ShiftJIS", then the "encoding" parameter is ignored and always set to "SHIFT-JIS".
     * If "_XMLWIDECHAR=1", then the "encoding" parameter is ignored and always set to "utf-16".
     * If no "encoding" parameter is given the "ISO-8859-1" encoding is used. */
    /** @} */

    /** @defgroup navigate Navigate the XMLNode structure
     * @ingroup XMLParserGeneral
     * @{ */
    XMLCSTR getName() const;                                       ///< name of the node
    XMLCSTR getText(int i=0) const;                                ///< return ith text field
    int nText() const;                                             ///< nbr of text field
    XMLNode getParentNode() const;                                 ///< return the parent node
    XMLNode getChildNode(int i=0) const;                           ///< return ith child node
    XMLNode getChildNode(XMLCSTR name, int i)  const;              ///< return ith child node with specific name (return an empty node if failing). If i==-1, this returns the last XMLNode with the given name.
    XMLNode getChildNode(XMLCSTR name, int *i=NULL) const;         ///< return next child node with specific name (return an empty node if failing)
    XMLNode getChildNodeWithAttribute(XMLCSTR tagName,
                                      XMLCSTR attributeName,
                                      XMLCSTR attributeValue=NULL,
                                      int *i=NULL)  const;         ///< return child node with specific name/attribute (return an empty node if failing)
    XMLNode getChildNodeByPath(XMLCSTR path, char createNodeIfMissing=0, XMLCHAR sep='/');
                                                                   ///< return the first child node with specific path
    XMLNode getChildNodeByPathNonConst(XMLSTR  path, char createNodeIfMissing=0, XMLCHAR sep='/');
                                                                   ///< return the first child node with specific path.

    int nChildNode(XMLCSTR name) const;                            ///< return the number of child node with specific name
    int nChildNode() const;                                        ///< nbr of child node
    XMLAttribute getAttribute(int i=0) const;                      ///< return ith attribute
    XMLCSTR      getAttributeName(int i=0) const;                  ///< return ith attribute name
    XMLCSTR      getAttributeValue(int i=0) const;                 ///< return ith attribute value
    char  isAttributeSet(XMLCSTR name) const;                      ///< test if an attribute with a specific name is given
    XMLCSTR getAttribute(XMLCSTR name, int i) const;               ///< return ith attribute content with specific name (return a NULL if failing)
    XMLCSTR getAttribute(XMLCSTR name, int *i=NULL) const;         ///< return next attribute content with specific name (return a NULL if failing)
    int nAttribute() const;                                        ///< nbr of attribute
    XMLClear getClear(int i=0) const;                              ///< return ith clear field (comments)
    int nClear() const;                                            ///< nbr of clear field
    XMLNodeContents enumContents(XMLElementPosition i) const;      ///< enumerate all the different contents (attribute,child,text, clear) of the current XMLNode. The order is reflecting the order of the original file/string. NOTE: 0 <= i < nElement();
    int nElement() const;                                          ///< nbr of different contents for current node
    char isEmpty() const;                                          ///< is this node Empty?
    char isDeclaration() const;                                    ///< is this node a declaration <? .... ?>
    XMLNode deepCopy() const;                                      ///< deep copy (duplicate/clone) a XMLNode
    static XMLNode emptyNode();                                    ///< return XMLNode::emptyXMLNode;
    /** @} */

    ~XMLNode();
    XMLNode(const XMLNode &A);                                     ///< to allow shallow/fast copy:
    XMLNode& operator=( const XMLNode& A );                        ///< to allow shallow/fast copy:

    XMLNode(): d(NULL){}
    static XMLNode emptyXMLNode;
    static XMLClear emptyXMLClear;
    static XMLAttribute emptyXMLAttribute;

    /** @defgroup xmlModify Create or Update the XMLNode structure
     * @ingroup XMLParserGeneral
     *  The functions in this group allows you to create from scratch (or update) a XMLNode structure. Start by creating your top
     *  node with the "createXMLTopNode" function and then add new nodes with the "addChild" function. The parameter 'pos' gives
     *  the position where the childNode, the text or the XMLClearTag will be inserted. The default value (pos=-1) inserts at the
     *  end. The value (pos=0) insert at the beginning (Insertion at the beginning is slower than at the end). <br>
     *
     *  REMARK: 0 <= pos < nChild()+nText()+nClear() <br>
     */

    /** @defgroup creation Creating from scratch a XMLNode structure
     * @ingroup xmlModify
     * @{ */
    static XMLNode createXMLTopNode(XMLCSTR lpszName, char isDeclaration=FALSE);                    ///< Create the top node of an XMLNode structure
    XMLNode        addChild(XMLCSTR lpszName, char isDeclaration=FALSE, XMLElementPosition pos=-1); ///< Add a new child node
    XMLNode        addChild(XMLNode nodeToAdd, XMLElementPosition pos=-1);                          ///< If the "nodeToAdd" has some parents, it will be detached from it's parents before being attached to the current XMLNode
    XMLAttribute  *addAttribute(XMLCSTR lpszName, XMLCSTR lpszValuev);                              ///< Add a new attribute
    XMLCSTR        addText(XMLCSTR lpszValue, XMLElementPosition pos=-1);                           ///< Add a new text content
    XMLClear      *addClear(XMLCSTR lpszValue, XMLCSTR lpszOpen=NULL, XMLCSTR lpszClose=NULL, XMLElementPosition pos=-1);
    /**< Add a new clear tag
     * @param lpszOpen default value "<![CDATA["
     * @param lpszClose default value "]]>"
     */
    /** @} */

    /** @defgroup xmlUpdate Updating Nodes
     * @ingroup xmlModify
     * Some update functions:
     * @{
     */
    XMLCSTR       updateName(XMLCSTR lpszName);                                                  ///< change node's name
    XMLAttribute *updateAttribute(XMLAttribute *newAttribute, XMLAttribute *oldAttribute);       ///< if the attribute to update is missing, a new one will be added
    XMLAttribute *updateAttribute(XMLCSTR lpszNewValue, XMLCSTR lpszNewName=NULL,int i=0);       ///< if the attribute to update is missing, a new one will be added
    XMLAttribute *updateAttribute(XMLCSTR lpszNewValue, XMLCSTR lpszNewName,XMLCSTR lpszOldName);///< set lpszNewName=NULL if you don't want to change the name of the attribute if the attribute to update is missing, a new one will be added
    XMLCSTR       updateText(XMLCSTR lpszNewValue, int i=0);                                     ///< if the text to update is missing, a new one will be added
    XMLCSTR       updateText(XMLCSTR lpszNewValue, XMLCSTR lpszOldValue);                        ///< if the text to update is missing, a new one will be added
    XMLClear     *updateClear(XMLCSTR lpszNewContent, int i=0);                                  ///< if the clearTag to update is missing, a new one will be added
    XMLClear     *updateClear(XMLClear *newP,XMLClear *oldP);                                    ///< if the clearTag to update is missing, a new one will be added
    XMLClear     *updateClear(XMLCSTR lpszNewValue, XMLCSTR lpszOldValue);                       ///< if the clearTag to update is missing, a new one will be added
    /** @} */

    /** @defgroup xmlDelete Deleting Nodes or Attributes
     * @ingroup xmlModify
     * Some deletion functions:
     * @{
     */
    /// The "deleteNodeContent" function forces the deletion of the content of this XMLNode and the subtree.
    void deleteNodeContent();
    /**< \note The XMLNode instances that are referring to the part of the subtree that has been deleted CANNOT be used anymore!!. Unexpected results will occur if you continue using them. */
    void deleteAttribute(int i=0);                   ///< Delete the ith attribute of the current XMLNode
    void deleteAttribute(XMLCSTR lpszName);          ///< Delete the attribute with the given name (the "strcmp" function is used to find the right attribute)
    void deleteAttribute(XMLAttribute *anAttribute); ///< Delete the attribute with the name "anAttribute->lpszName" (the "strcmp" function is used to find the right attribute)
    void deleteText(int i=0);                        ///< Delete the Ith text content of the current XMLNode
    void deleteText(XMLCSTR lpszValue);              ///< Delete the text content "lpszValue" inside the current XMLNode (direct "pointer-to-pointer" comparison is used to find the right text)
    void deleteClear(int i=0);                       ///< Delete the Ith clear tag inside the current XMLNode
    void deleteClear(XMLCSTR lpszValue);             ///< Delete the clear tag "lpszValue" inside the current XMLNode (direct "pointer-to-pointer" comparison is used to find the clear tag)
    void deleteClear(XMLClear *p);                   ///< Delete the clear tag "p" inside the current XMLNode (direct "pointer-to-pointer" comparison on the lpszName of the clear tag is used to find the clear tag)
    /** @} */

    /** @defgroup xmlWOSD ???_WOSD functions.
     * @ingroup xmlModify
     *  The strings given as parameters for the "add" and "update" methods that have a name with
     *  the postfix "_WOSD" (that means "WithOut String Duplication")(for example "addText_WOSD")
     *  will be free'd by the XMLNode class. For example, it means that this is incorrect:
     *  \code
     *     xNode.addText_WOSD("foo");
     *     xNode.updateAttribute_WOSD("#newcolor" ,NULL,"color");
     *  \endcode
     *  In opposition, this is correct:
     *  \code
     *     xNode.addText("foo");
     *     xNode.addText_WOSD(stringDup("foo"));
     *     xNode.updateAttribute("#newcolor" ,NULL,"color");
     *     xNode.updateAttribute_WOSD(stringDup("#newcolor"),NULL,"color");
     *  \endcode
     *  Typically, you will never do:
     *  \code
     *     char *b=(char*)malloc(...);
     *     xNode.addText(b);
     *     free(b);
     *  \endcode
     *  ... but rather:
     *  \code
     *     char *b=(char*)malloc(...);
     *     xNode.addText_WOSD(b);
     *  \endcode
     *  ('free(b)' is performed by the XMLNode class)
     * @{ */
    static XMLNode createXMLTopNode_WOSD(XMLSTR lpszName, char isDeclaration=FALSE);                     ///< Create the top node of an XMLNode structure
    XMLNode        addChild_WOSD(XMLSTR lpszName, char isDeclaration=FALSE, XMLElementPosition pos=-1);  ///< Add a new child node
    XMLAttribute  *addAttribute_WOSD(XMLSTR lpszName, XMLSTR lpszValue);                                 ///< Add a new attribute
    XMLCSTR        addText_WOSD(XMLSTR lpszValue, XMLElementPosition pos=-1);                            ///< Add a new text content
    XMLClear      *addClear_WOSD(XMLSTR lpszValue, XMLCSTR lpszOpen=NULL, XMLCSTR lpszClose=NULL, XMLElementPosition pos=-1); ///< Add a new clear Tag

    XMLCSTR        updateName_WOSD(XMLSTR lpszName);                                                  ///< change node's name
    XMLAttribute  *updateAttribute_WOSD(XMLAttribute *newAttribute, XMLAttribute *oldAttribute);      ///< if the attribute to update is missing, a new one will be added
    XMLAttribute  *updateAttribute_WOSD(XMLSTR lpszNewValue, XMLSTR lpszNewName=NULL,int i=0);        ///< if the attribute to update is missing, a new one will be added
    XMLAttribute  *updateAttribute_WOSD(XMLSTR lpszNewValue, XMLSTR lpszNewName,XMLCSTR lpszOldName); ///< set lpszNewName=NULL if you don't want to change the name of the attribute if the attribute to update is missing, a new one will be added
    XMLCSTR        updateText_WOSD(XMLSTR lpszNewValue, int i=0);                                     ///< if the text to update is missing, a new one will be added
    XMLCSTR        updateText_WOSD(XMLSTR lpszNewValue, XMLCSTR lpszOldValue);                        ///< if the text to update is missing, a new one will be added
    XMLClear      *updateClear_WOSD(XMLSTR lpszNewContent, int i=0);                                  ///< if the clearTag to update is missing, a new one will be added
    XMLClear      *updateClear_WOSD(XMLClear *newP,XMLClear *oldP);                                   ///< if the clearTag to update is missing, a new one will be added
    XMLClear      *updateClear_WOSD(XMLSTR lpszNewValue, XMLCSTR lpszOldValue);                       ///< if the clearTag to update is missing, a new one will be added
    /** @} */

    /** @defgroup xmlPosition Position helper functions (use in conjunction with the update&add functions
     * @ingroup xmlModify
     * These are some useful functions when you want to insert a childNode, a text or a XMLClearTag in the
     * middle (at a specified position) of a XMLNode tree already constructed. The value returned by these
     * methods is to be used as last parameter (parameter 'pos') of addChild, addText or addClear.
     * @{ */
    XMLElementPosition positionOfText(int i=0) const;
    XMLElementPosition positionOfText(XMLCSTR lpszValue) const;
    XMLElementPosition positionOfClear(int i=0) const;
    XMLElementPosition positionOfClear(XMLCSTR lpszValue) const;
    XMLElementPosition positionOfClear(XMLClear *a) const;
    XMLElementPosition positionOfChildNode(int i=0) const;
    XMLElementPosition positionOfChildNode(XMLNode x) const;
    XMLElementPosition positionOfChildNode(XMLCSTR name, int i=0) const; ///< return the position of the ith childNode with the specified name if (name==NULL) return the position of the ith childNode
    /** @} */

    /// Enumeration for XML character encoding.
    typedef enum XMLCharEncoding
    {
        char_encoding_error=0,
        char_encoding_UTF8=1,
        char_encoding_legacy=2,
        char_encoding_ShiftJIS=3,
        char_encoding_GB2312=4,
        char_encoding_Big5=5,
        char_encoding_GBK=6     // this is actually the same as Big5
    } XMLCharEncoding;

    /** \addtogroup conversions
     * @{ */

    /// Sets the global options for the conversions
    static char setGlobalOptions(XMLCharEncoding characterEncoding=XMLNode::char_encoding_UTF8, char guessWideCharChars=1,
                                 char dropWhiteSpace=1, char removeCommentsInMiddleOfText=1);
    /**< The "setGlobalOptions" function allows you to change four global parameters that affect string & file
     * parsing. First of all, you most-probably will never have to change these 3 global parameters.
     *
     * @param guessWideCharChars If "guessWideCharChars"=1 and if this library is compiled in WideChar mode, then the
     *     XMLNode::parseFile and XMLNode::openFileHelper functions will test if the file contains ASCII
     *     characters. If this is the case, then the file will be loaded and converted in memory to
     *     WideChar before being parsed. If 0, no conversion will be performed.
     *
     * @param guessWideCharChars If "guessWideCharChars"=1 and if this library is compiled in ASCII/UTF8/char* mode, then the
     *     XMLNode::parseFile and XMLNode::openFileHelper functions will test if the file contains WideChar
     *     characters. If this is the case, then the file will be loaded and converted in memory to
     *     ASCII/UTF8/char* before being parsed. If 0, no conversion will be performed.
     *
     * @param characterEncoding This parameter is only meaningful when compiling in char* mode (multibyte character mode).
     *     In wchar_t* (wide char mode), this parameter is ignored. This parameter should be one of the
     *     three currently recognized encodings: XMLNode::encoding_UTF8, XMLNode::encoding_ascii,
     *     XMLNode::encoding_ShiftJIS.
     *
     * @param dropWhiteSpace In most situations, text fields containing only white spaces (and carriage returns)
     *     are useless. Even more, these "empty" text fields are annoying because they increase the
     *     complexity of the user's code for parsing. So, 99% of the time, it's better to drop
     *     the "empty" text fields. However The XML specification indicates that no white spaces
     *     should be lost when parsing the file. So to be perfectly XML-compliant, you should set
     *     dropWhiteSpace=0. A note of caution: if you set "dropWhiteSpace=0", the parser will be
     *     slower and your code will be more complex.
     *
     * @param removeCommentsInMiddleOfText To explain this parameter, let's consider this code:
     * \code
     *        XMLNode x=XMLNode::parseString("<a>foo<!-- hello -->bar<!DOCTYPE world >chu</a>","a");
     * \endcode
     *     If removeCommentsInMiddleOfText=0, then we will have:
     * \code
     *        x.getText(0) -> "foo"
     *        x.getText(1) -> "bar"
     *        x.getText(2) -> "chu"
     *        x.getClear(0) --> "<!-- hello -->"
     *        x.getClear(1) --> "<!DOCTYPE world >"
     * \endcode
     *     If removeCommentsInMiddleOfText=1, then we will have:
     * \code
     *        x.getText(0) -> "foobar"
     *        x.getText(1) -> "chu"
     *        x.getClear(0) --> "<!DOCTYPE world >"
     * \endcode
     *
     * \return "0" when there are no errors. If you try to set an unrecognized encoding then the return value will be "1" to signal an error.
     *
     * \note Sometime, it's useful to set "guessWideCharChars=0" to disable any conversion
     * because the test to detect the file-type (ASCII/UTF8/char* or WideChar) may fail (rarely). */

    /// Guess the character encoding of the string (ascii, utf8 or shift-JIS)
    static XMLCharEncoding guessCharEncoding(void *buffer, int bufLen, char useXMLEncodingAttribute=1);
    /**< The "guessCharEncoding" function try to guess the character encoding. You most-probably will never
     * have to use this function. It then returns the appropriate value of the global parameter
     * "characterEncoding" described in the XMLNode::setGlobalOptions. The guess is based on the content of a buffer of length
     * "bufLen" bytes that contains the first bytes (minimum 25 bytes; 200 bytes is a good value) of the
     * file to be parsed. The XMLNode::openFileHelper function is using this function to automatically compute
     * the value of the "characterEncoding" global parameter. There are several heuristics used to do the
     * guess. One of the heuristic is based on the "encoding" attribute. The original XML specifications
     * forbids to use this attribute to do the guess but you can still use it if you set
     * "useXMLEncodingAttribute" to 1 (this is the default behavior and the behavior of most parsers).
     * If an inconsistency in the encoding is detected, then the return value is "0". */
    /** @} */

  private:
      // these are functions and structures used internally by the XMLNode class (don't bother about them):

      typedef struct XMLNodeDataTag // to allow shallow copy and "intelligent/smart" pointers (automatic delete):
      {
          XMLCSTR                lpszName;        // Element name (=NULL if root)
          int                    nChild,          // Number of child nodes
                                 nText,           // Number of text fields
                                 nClear,          // Number of Clear fields (comments)
                                 nAttribute;      // Number of attributes
          char                   isDeclaration;   // Whether node is an XML declaration - '<?xml ?>'
          struct XMLNodeDataTag  *pParent;        // Pointer to parent element (=NULL if root)
          XMLNode                *pChild;         // Array of child nodes
          XMLCSTR                *pText;          // Array of text fields
          XMLClear               *pClear;         // Array of clear fields
          XMLAttribute           *pAttribute;     // Array of attributes
          int                    *pOrder;         // order of the child_nodes,text_fields,clear_fields
          int                    ref_count;       // for garbage collection (smart pointers)
      } XMLNodeData;
      XMLNodeData *d;

      char parseClearTag(void *px, void *pa);
      char maybeAddTxT(void *pa, XMLCSTR tokenPStr);
      int ParseXMLElement(void *pXML);
      void *addToOrder(int memInc, int *_pos, int nc, void *p, int size, XMLElementType xtype);
      int indexText(XMLCSTR lpszValue) const;
      int indexClear(XMLCSTR lpszValue) const;
      XMLNode addChild_priv(int,XMLSTR,char,int);
      XMLAttribute *addAttribute_priv(int,XMLSTR,XMLSTR);
      XMLCSTR addText_priv(int,XMLSTR,int);
      XMLClear *addClear_priv(int,XMLSTR,XMLCSTR,XMLCSTR,int);
      void emptyTheNode(char force);
      static inline XMLElementPosition findPosition(XMLNodeData *d, int index, XMLElementType xtype);
      static int CreateXMLStringR(XMLNodeData *pEntry, XMLSTR lpszMarker, int nFormat);
      static int removeOrderElement(XMLNodeData *d, XMLElementType t, int index);
      static void exactMemory(XMLNodeData *d);
      static int detachFromParent(XMLNodeData *d);
} XMLNode;

/// This structure is given by the function XMLNode::enumContents.
typedef struct XMLNodeContents
{
    /// This dictates what's the content of the XMLNodeContent
    enum XMLElementType etype;
    /**< should be an union to access the appropriate data. Compiler does not allow union of object with constructor... too bad. */
    XMLNode child;
    XMLAttribute attrib;
    XMLCSTR text;
    XMLClear clear;

} XMLNodeContents;

/** @defgroup StringAlloc String Allocation/Free functions
 * @ingroup xmlModify
 * @{ */
/// Duplicate (copy in a new allocated buffer) the source string.
XMLDLLENTRY XMLSTR stringDup(XMLCSTR source, int cbData=-1);
/**< This is
 * a very handy function when used with all the "XMLNode::*_WOSD" functions (\link xmlWOSD \endlink).
 * @param cbData If !=0 then cbData is the number of chars to duplicate. New strings allocated with
 * this function should be free'd using the "freeXMLString" function. */

/// to free the string allocated inside the "stringDup" function or the "createXMLString" function.
XMLDLLENTRY void freeXMLString(XMLSTR t); // {free(t);}
/** @} */

/** @defgroup atoX ato? like functions
 * @ingroup XMLParserGeneral
 * The "xmlto?" functions are equivalents to the atoi, atol, atof functions.
 * The only difference is: If the variable "xmlString" is NULL, than the return value
 * is "defautValue". These 6 functions are only here as "convenience" functions for the
 * user (they are not used inside the XMLparser). If you don't need them, you can
 * delete them without any trouble.
 *
 * @{ */
XMLDLLENTRY char    xmltob(XMLCSTR xmlString,char   defautValue=0);
XMLDLLENTRY int     xmltoi(XMLCSTR xmlString,int    defautValue=0);
XMLDLLENTRY long    xmltol(XMLCSTR xmlString,long   defautValue=0);
XMLDLLENTRY double  xmltof(XMLCSTR xmlString,double defautValue=.0);
XMLDLLENTRY XMLCSTR xmltoa(XMLCSTR xmlString,XMLCSTR defautValue=_CXML(""));
XMLDLLENTRY XMLCHAR xmltoc(XMLCSTR xmlString,XMLCHAR defautValue=_CXML('\0'));
/** @} */

/** @defgroup ToXMLStringTool Helper class to create XML files using "printf", "fprintf", "cout",... functions.
 * @ingroup XMLParserGeneral
 * @{ */
/// Helper class to create XML files using "printf", "fprintf", "cout",... functions.
/** The ToXMLStringTool class helps you creating XML files using "printf", "fprintf", "cout",... functions.
 * The "ToXMLStringTool" class is processing strings so that all the characters
 * &,",',<,> are replaced by their XML equivalent:
 * \verbatim &amp;, &quot;, &apos;, &lt;, &gt; \endverbatim
 * Using the "ToXMLStringTool class" and the "fprintf function" is THE most efficient
 * way to produce VERY large XML documents VERY fast.
 * \note If you are creating from scratch an XML file using the provided XMLNode class
 * you must not use the "ToXMLStringTool" class (because the "XMLNode" class does the
 * processing job for you during rendering).*/
typedef struct XMLDLLENTRY ToXMLStringTool
{
public:
    ToXMLStringTool(): buf(NULL),buflen(0){}
    ~ToXMLStringTool();
    void freeBuffer();///<call this function when you have finished using this object to release memory used by the internal buffer.

    XMLSTR toXML(XMLCSTR source);///< returns a pointer to an internal buffer that contains a XML-encoded string based on the "source" parameter.

    /** The "toXMLUnSafe" function is deprecated because there is a possibility of
     * "destination-buffer-overflow". It converts the string
     * "source" to the string "dest". */
    static XMLSTR toXMLUnSafe(XMLSTR dest,XMLCSTR source); ///< deprecated: use "toXML" instead
    static int lengthXMLString(XMLCSTR source);            ///< deprecated: use "toXML" instead

private:
    XMLSTR buf;
    int buflen;
} ToXMLStringTool;
/** @} */

/** @defgroup XMLParserBase64Tool Helper class to include binary data inside XML strings using "Base64 encoding".
 * @ingroup XMLParserGeneral
 * @{ */
/// Helper class to include binary data inside XML strings using "Base64 encoding".
/** The "XMLParserBase64Tool" class allows you to include any binary data (images, sounds,...)
 * into an XML document using "Base64 encoding". This class is completely
 * separated from the rest of the xmlParser library and can be removed without any problem.
 * To include some binary data into an XML file, you must convert the binary data into
 * standard text (using "encode"). To retrieve the original binary data from the
 * b64-encoded text included inside the XML file, use "decode". Alternatively, these
 * functions can also be used to "encrypt/decrypt" some critical data contained inside
 * the XML (it's not a strong encryption at all, but sometimes it can be useful). */
typedef struct XMLDLLENTRY XMLParserBase64Tool
{
public:
    XMLParserBase64Tool(): buf(NULL),buflen(0){}
    ~XMLParserBase64Tool();
    void freeBuffer();///< Call this function when you have finished using this object to release memory used by the internal buffer.

    /**
     * @param formatted If "formatted"=true, some space will be reserved for a carriage-return every 72 chars. */
    static int encodeLength(int inBufLen, char formatted=0); ///< return the length of the base64 string that encodes a data buffer of size inBufLen bytes.

    /**
     * The "base64Encode" function returns a string containing the base64 encoding of "inByteLen" bytes
     * from "inByteBuf". If "formatted" parameter is true, then there will be a carriage-return every 72 chars.
     * The string will be free'd when the XMLParserBase64Tool object is deleted.
     * All returned strings are sharing the same memory space. */
    XMLSTR encode(unsigned char *inByteBuf, unsigned int inByteLen, char formatted=0); ///< returns a pointer to an internal buffer containing the base64 string containing the binary data encoded from "inByteBuf"

    /// returns the number of bytes which will be decoded from "inString".
    static unsigned int decodeSize(XMLCSTR inString, XMLError *xe=NULL);

    /**
     * The "decode" function returns a pointer to a buffer containing the binary data decoded from "inString"
     * The output buffer will be free'd when the XMLParserBase64Tool object is deleted.
     * All output buffer are sharing the same memory space.
     * @param inString If "instring" is malformed, NULL will be returned */
    unsigned char* decode(XMLCSTR inString, int *outByteLen=NULL, XMLError *xe=NULL); ///< returns a pointer to an internal buffer containing the binary data decoded from "inString"

    /**
     * decodes data from "inString" to "outByteBuf". You need to provide the size (in byte) of "outByteBuf"
     * in "inMaxByteOutBuflen". If "outByteBuf" is not large enough or if data is malformed, then "FALSE"
     * will be returned; otherwise "TRUE". */
    static unsigned char decode(XMLCSTR inString, unsigned char *outByteBuf, int inMaxByteOutBuflen, XMLError *xe=NULL); ///< deprecated.

private:
    void *buf;
    int buflen;
    void alloc(int newsize);
}XMLParserBase64Tool;
/** @} */

#undef XMLDLLENTRY

#endif
