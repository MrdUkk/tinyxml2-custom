#include "stdafx.h"
/*
Original code by Lee Thomason (www.grinninglizard.com)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#include "tinyxml2.h"

#include <new>		// yes, this one new style header, is in the Android SDK.
#if defined(ANDROID_NDK) || defined(__QNXNTO__)
#   include <stddef.h>
#else
#   include <cstddef>
#endif

static const char LINE_FEED				= (char)0x0a;			// all line endings are normalized to LF
static const char LF = LINE_FEED;
static const char CARRIAGE_RETURN		= (char)0x0d;			// CR gets filtered out
static const char CR = CARRIAGE_RETURN;
static const char SINGLE_QUOTE			= '\'';
static const char DOUBLE_QUOTE			= '\"';

// Bunch of unicode info at:
//		http://www.unicode.org/faq/utf_bom.html
//	ef bb bf (Microsoft "lead bytes") - designates UTF-8

static const unsigned char TIXML_UTF_LEAD_0 = 0xefU;
static const unsigned char TIXML_UTF_LEAD_1 = 0xbbU;
static const unsigned char TIXML_UTF_LEAD_2 = 0xbfU;

namespace tinyxml2
{

struct Entity {
    const wchar_t* pattern;
    int length;
    wchar_t value;
};

static const int NUM_ENTITIES = 5;
static const Entity entities[NUM_ENTITIES] = {
    { L"quot", 4,	DOUBLE_QUOTE },
    { L"amp", 3,		'&'  },
    { L"apos", 4,	SINGLE_QUOTE },
    { L"lt",	2, 		'<'	 },
    { L"gt",	2,		'>'	 }
};


StrPair::~StrPair()
{
    Reset();
}


void StrPair::TransferTo( StrPair* other )
{
    if ( this == other ) {
        return;
    }
    // This in effect implements the assignment operator by "moving"
    // ownership (as in auto_ptr).

    TIXMLASSERT( other->_flags == 0 );
    TIXMLASSERT( other->_start == 0 );
    TIXMLASSERT( other->_end == 0 );

    other->Reset();

    other->_flags = _flags;
    other->_start = _start;
    other->_end = _end;

    _flags = 0;
    _start = 0;
    _end = 0;
}

void StrPair::Reset()
{
    if ( _flags & NEEDS_DELETE ) {
        delete [] _start;
    }
    _flags = 0;
    _start = 0;
    _end = 0;
}


void StrPair::SetStr( const wchar_t* str, int flags )
{
    Reset();
	size_t len = wcslen(str);
    _start = new wchar_t[ len+1 ];
    wmemcpy( _start, str, len+1 );
    _end = _start + len;
    _flags = flags | NEEDS_DELETE;
}


wchar_t* StrPair::ParseText( wchar_t* p, const wchar_t* endTag, int strFlags )
{
    TIXMLASSERT( endTag && *endTag );

    wchar_t* start = p;
    wchar_t  endChar = *endTag;
	size_t length = wcslen(endTag);

    // Inner loop of text parsing.
    while ( *p ) {
		if (*p == endChar && wcsncmp(p, endTag, length) == 0) {
            Set( start, p, strFlags );
            return p + length;
        }
        ++p;
    }
    return 0;
}


wchar_t* StrPair::ParseName( wchar_t* p )
{
    if ( !p || !(*p) ) {
        return 0;
    }

    wchar_t* const start = p;

    while( *p && ( p == start ? XMLUtil::IsNameStartChar( *p ) : XMLUtil::IsNameChar( *p ) )) {
        ++p;
    }

    if ( p > start ) {
        Set( start, p, 0 );
        return p;
    }
    return 0;
}


void StrPair::CollapseWhitespace()
{
    // Adjusting _start would cause undefined behavior on delete[]
    TIXMLASSERT( ( _flags & NEEDS_DELETE ) == 0 );
    // Trim leading space.
    _start = XMLUtil::SkipWhiteSpace( _start );

    if ( _start && *_start ) {
        wchar_t* p = _start;	// the read pointer
        wchar_t* q = _start;	// the write pointer

        while( *p ) {
            if ( XMLUtil::IsWhiteSpace( *p )) {
                p = XMLUtil::SkipWhiteSpace( p );
                if ( *p == 0 ) {
                    break;    // don't write to q; this trims the trailing space.
                }
                *q = ' ';
                ++q;
            }
            *q = *p;
            ++q;
            ++p;
        }
        *q = 0;
    }
}


const wchar_t* StrPair::GetStr()
{
    if ( _flags & NEEDS_FLUSH ) {
        *_end = 0;
        _flags ^= NEEDS_FLUSH;

        if ( _flags ) {
            wchar_t* p = _start;	// the read pointer
            wchar_t* q = _start;	// the write pointer

            while( p < _end ) {
                if ( (_flags & NEEDS_NEWLINE_NORMALIZATION) && *p == CR ) {
                    // CR-LF pair becomes LF
                    // CR alone becomes LF
                    // LF-CR becomes LF
                    if ( *(p+1) == LF ) {
                        p += 2;
                    }
                    else {
                        ++p;
                    }
                    *q++ = LF;
                }
                else if ( (_flags & NEEDS_NEWLINE_NORMALIZATION) && *p == LF ) {
                    if ( *(p+1) == CR ) {
                        p += 2;
                    }
                    else {
                        ++p;
                    }
                    *q++ = LF;
                }
                else if ( (_flags & NEEDS_ENTITY_PROCESSING) && *p == '&' ) {
                    // Entities handled by tinyXML2:
                    // - special entities in the entity table [in/out]
                    // - numeric character reference [in]
                    //   &#20013; or &#x4e2d;

                    if ( *(p+1) == '#' ) {
                        const int buflen = 10;
                        wchar_t buf[buflen] = { 0 };
                        int len = 0;
                        p = const_cast<wchar_t*>( XMLUtil::GetCharacterRef( p, buf, &len ) );
                        TIXMLASSERT( 0 <= len && len <= buflen );
                        TIXMLASSERT( q + len <= p );
                        memcpy( q, buf, len );
                        q += len;
                    }
                    else {
                        int i=0;
                        for(; i<NUM_ENTITIES; ++i ) {
                            const Entity& entity = entities[i];
							if (wcsncmp(p + 1, entity.pattern, entity.length) == 0
                                    && *( p + entity.length + 1 ) == ';' ) {
                                // Found an entity - convert.
                                *q = entity.value;
                                ++q;
                                p += entity.length + 2;
                                break;
                            }
                        }
                        if ( i == NUM_ENTITIES ) {
                            // fixme: treat as error?
                            ++p;
                            ++q;
                        }
                    }
                }
                else {
                    *q = *p;
                    ++p;
                    ++q;
                }
            }
            *q = 0;
        }
        // The loop below has plenty going on, and this
        // is a less useful mode. Break it out.
        if ( _flags & COLLAPSE_WHITESPACE ) {
            CollapseWhitespace();
        }
        _flags = (_flags & NEEDS_DELETE);
    }
    return _start;
}




// --------- XMLUtil ----------- //

const wchar_t* XMLUtil::ReadBOM( const wchar_t* p, bool* bom )
{
    *bom = false;
    const wchar_t* pu = (p);
    // Check for BOM:
    if (    *(pu+0) == TIXML_UTF_LEAD_0
            && *(pu+1) == TIXML_UTF_LEAD_1
            && *(pu+2) == TIXML_UTF_LEAD_2 ) {
        *bom = true;
        p += 3;
    }
	else if (*(pu + 0) == 0xffU 
			&& *(pu + 1) == 0xfeU) 
	{
		*bom = true;
		p += 2;
	}
    return p;
}


void XMLUtil::ConvertUTF32ToUTF8( unsigned long input, wchar_t* output, int* length )
{
    const unsigned long BYTE_MASK = 0xBF;
    const unsigned long BYTE_MARK = 0x80;
    const unsigned long FIRST_BYTE_MARK[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

    if (input < 0x80) {
        *length = 1;
    }
    else if ( input < 0x800 ) {
        *length = 2;
    }
    else if ( input < 0x10000 ) {
        *length = 3;
    }
    else if ( input < 0x200000 ) {
        *length = 4;
    }
    else {
        *length = 0;    // This code won't covert this correctly anyway.
        return;
    }

    output += *length;

    // Scary scary fall throughs.
    switch (*length) {
        case 4:
            --output;
            *output = (char)((input | BYTE_MARK) & BYTE_MASK);
            input >>= 6;
        case 3:
            --output;
            *output = (char)((input | BYTE_MARK) & BYTE_MASK);
            input >>= 6;
        case 2:
            --output;
            *output = (char)((input | BYTE_MARK) & BYTE_MASK);
            input >>= 6;
        case 1:
            --output;
            *output = (char)(input | FIRST_BYTE_MARK[*length]);
        default:
            break;
    }
}


const wchar_t* XMLUtil::GetCharacterRef( const wchar_t* p, wchar_t* value, int* length )
{
    // Presume an entity, and pull it out.
    *length = 0;

    if ( *(p+1) == '#' && *(p+2) ) {
        unsigned long ucs = 0;
        ptrdiff_t delta = 0;
        unsigned mult = 1;

        if ( *(p+2) == 'x' ) {
            // Hexadecimal.
            if ( !*(p+3) ) {
                return 0;
            }

            const wchar_t* q = p+3;
			q = wcschr(q, ';');

            if ( !q || !*q ) {
                return 0;
            }

            delta = q-p;
            --q;

            while ( *q != 'x' ) {
                if ( *q >= '0' && *q <= '9' ) {
                    ucs += mult * (*q - '0');
                }
                else if ( *q >= 'a' && *q <= 'f' ) {
                    ucs += mult * (*q - 'a' + 10);
                }
                else if ( *q >= 'A' && *q <= 'F' ) {
                    ucs += mult * (*q - 'A' + 10 );
                }
                else {
                    return 0;
                }
                mult *= 16;
                --q;
            }
        }
        else {
            // Decimal.
            if ( !*(p+2) ) {
                return 0;
            }

            const wchar_t* q = p+2;
			q = wcschr(q, ';');

            if ( !q || !*q ) {
                return 0;
            }

            delta = q-p;
            --q;

            while ( *q != '#' ) {
                if ( *q >= '0' && *q <= '9' ) {
                    ucs += mult * (*q - '0');
                }
                else {
                    return 0;
                }
                mult *= 10;
                --q;
            }
        }
        // convert the UCS to UTF-8
        ConvertUTF32ToUTF8( ucs, value, length );
        return p + delta + 1;
    }
    return p+1;
}


void XMLUtil::ToStr( int v, wchar_t* buffer, int bufferSize )
{
    TIXML_SNPRINTF( buffer, bufferSize, L"%d", v );
}


void XMLUtil::ToStr( unsigned v, wchar_t* buffer, int bufferSize )
{
    TIXML_SNPRINTF( buffer, bufferSize, L"%u", v );
}


void XMLUtil::ToStr( bool v, wchar_t* buffer, int bufferSize )
{
    TIXML_SNPRINTF( buffer, bufferSize, L"%d", v ? 1 : 0 );
}

/*
	ToStr() of a number is a very tricky topic.
	https://github.com/leethomason/tinyxml2/issues/106
*/
void XMLUtil::ToStr( float v, wchar_t* buffer, int bufferSize )
{
    TIXML_SNPRINTF( buffer, bufferSize, L"%.8g", v );
}


void XMLUtil::ToStr( double v, wchar_t* buffer, int bufferSize )
{
    TIXML_SNPRINTF( buffer, bufferSize, L"%.17g", v );
}

void XMLUtil::ToStr(__int64 v, wchar_t* buffer, int bufferSize)
{
	TIXML_SNPRINTF(buffer, bufferSize, L"%I64d", v);
}

bool XMLUtil::ToInt( const wchar_t* str, int* value )
{
    if ( TIXML_SSCANF( str, L"%d", value ) == 1 ) {
        return true;
    }
    return false;
}

bool XMLUtil::ToUnsigned( const wchar_t* str, unsigned *value )
{
    if ( TIXML_SSCANF( str, L"%u", value ) == 1 ) {
        return true;
    }
    return false;
}

bool XMLUtil::ToBool( const wchar_t* str, bool* value )
{
    int ival = 0;
    if ( ToInt( str, &ival )) {
        *value = (ival==0) ? false : true;
        return true;
    }
	if (StringEqual(str, L"true") || StringEqual(str, L"t")) {
        *value = true;
        return true;
    }
	else if (StringEqual(str, L"false") || StringEqual(str, L"f")) {
        *value = false;
        return true;
    }
    return false;
}


bool XMLUtil::ToFloat( const wchar_t* str, float* value )
{
    if ( TIXML_SSCANF( str, L"%f", value ) == 1 ) {
        return true;
    }
    return false;
}

bool XMLUtil::ToDouble( const wchar_t* str, double* value )
{
    if ( TIXML_SSCANF( str, L"%lf", value ) == 1 ) {
        return true;
    }
    return false;
}


wchar_t* TiXMLDocument::Identify( wchar_t* p, XMLNode** node )
{
    wchar_t* const start = p;
    p = XMLUtil::SkipWhiteSpace( p );
    if( !p || !*p ) {
        return p;
    }

    // What is this thing?
	// These strings define the matching patters:
    static const wchar_t* xmlHeader		= { L"<?" };
    static const wchar_t* commentHeader	= { L"<!--" };
    static const wchar_t* dtdHeader		= { L"<!" };
    static const wchar_t* cdataHeader	= { L"<![CDATA[" };
    static const wchar_t* elementHeader	= { L"<" };	// and a header for everything else; check last.

    static const int xmlHeaderLen		= 2;
    static const int commentHeaderLen	= 4;
    static const int dtdHeaderLen		= 2;
    static const int cdataHeaderLen		= 9;
    static const int elementHeaderLen	= 1;

#if defined(_MSC_VER)
#pragma warning ( push )
#pragma warning ( disable : 4127 )
#endif
    TIXMLASSERT( sizeof( XMLComment ) == sizeof( XMLUnknown ) );		// use same memory pool
    TIXMLASSERT( sizeof( XMLComment ) == sizeof( XMLDeclaration ) );	// use same memory pool
#if defined(_MSC_VER)
#pragma warning (pop)
#endif
    XMLNode* returnNode = 0;
    if ( XMLUtil::StringEqual( p, xmlHeader, xmlHeaderLen ) ) {
        returnNode = new (_commentPool.Alloc()) XMLDeclaration( this );
        returnNode->_memPool = &_commentPool;
        p += xmlHeaderLen;
    }
    else if ( XMLUtil::StringEqual( p, commentHeader, commentHeaderLen ) ) {
        returnNode = new (_commentPool.Alloc()) XMLComment( this );
        returnNode->_memPool = &_commentPool;
        p += commentHeaderLen;
    }
    else if ( XMLUtil::StringEqual( p, cdataHeader, cdataHeaderLen ) ) {
        XMLText* text = new (_textPool.Alloc()) XMLText( this );
        returnNode = text;
        returnNode->_memPool = &_textPool;
        p += cdataHeaderLen;
        text->SetCData( true );
    }
    else if ( XMLUtil::StringEqual( p, dtdHeader, dtdHeaderLen ) ) {
        returnNode = new (_commentPool.Alloc()) XMLUnknown( this );
        returnNode->_memPool = &_commentPool;
        p += dtdHeaderLen;
    }
    else if ( XMLUtil::StringEqual( p, elementHeader, elementHeaderLen ) ) {
        returnNode = new (_elementPool.Alloc()) XMLElement( this );
        returnNode->_memPool = &_elementPool;
        p += elementHeaderLen;
    }
    else {
        returnNode = new (_textPool.Alloc()) XMLText( this );
        returnNode->_memPool = &_textPool;
        p = start;	// Back it up, all the text counts.
    }

    *node = returnNode;
    return p;
}


bool TiXMLDocument::Accept( XMLVisitor* visitor ) const
{
    if ( visitor->VisitEnter( *this ) ) {
        for ( const XMLNode* node=FirstChild(); node; node=node->NextSibling() ) {
            if ( !node->Accept( visitor ) ) {
                break;
            }
        }
    }
    return visitor->VisitExit( *this );
}


// --------- XMLNode ----------- //

XMLNode::XMLNode( TiXMLDocument* doc ) :
    _document( doc ),
    _parent( 0 ),
    _firstChild( 0 ), _lastChild( 0 ),
    _prev( 0 ), _next( 0 ),
    _memPool( 0 )
{
}


XMLNode::~XMLNode()
{
    DeleteChildren();
    if ( _parent ) {
        _parent->Unlink( this );
    }
}

const wchar_t* XMLNode::Value() const 
{
    return _value.GetStr();
}

void XMLNode::SetValue( const wchar_t* str, bool staticMem )
{
    if ( staticMem ) {
        _value.SetInternedStr( str );
    }
    else {
        _value.SetStr( str );
    }
}


void XMLNode::DeleteChildren()
{
    while( _firstChild ) {
        XMLNode* node = _firstChild;
        Unlink( node );

        DeleteNode( node );
    }
    _firstChild = _lastChild = 0;
}


void XMLNode::Unlink( XMLNode* child )
{
    if ( child == _firstChild ) {
        _firstChild = _firstChild->_next;
    }
    if ( child == _lastChild ) {
        _lastChild = _lastChild->_prev;
    }

    if ( child->_prev ) {
        child->_prev->_next = child->_next;
    }
    if ( child->_next ) {
        child->_next->_prev = child->_prev;
    }
	child->_parent = 0;
}


void XMLNode::DeleteChild( XMLNode* node )
{
    TIXMLASSERT( node->_parent == this );
    DeleteNode( node );
}


XMLNode* XMLNode::InsertEndChild( XMLNode* addThis )
{
	if (addThis->_document != _document)
		return 0;

	if (addThis->_parent)
		addThis->_parent->Unlink( addThis );
	else
	   addThis->_memPool->SetTracked();

    if ( _lastChild ) {
        TIXMLASSERT( _firstChild );
        TIXMLASSERT( _lastChild->_next == 0 );
        _lastChild->_next = addThis;
        addThis->_prev = _lastChild;
        _lastChild = addThis;

        addThis->_next = 0;
    }
    else {
        TIXMLASSERT( _firstChild == 0 );
        _firstChild = _lastChild = addThis;

        addThis->_prev = 0;
        addThis->_next = 0;
    }
    addThis->_parent = this;
    return addThis;
}


XMLNode* XMLNode::InsertFirstChild( XMLNode* addThis )
{
	if (addThis->_document != _document)
		return 0;

	if (addThis->_parent)
		addThis->_parent->Unlink( addThis );
	else
	   addThis->_memPool->SetTracked();

    if ( _firstChild ) {
        TIXMLASSERT( _lastChild );
        TIXMLASSERT( _firstChild->_prev == 0 );

        _firstChild->_prev = addThis;
        addThis->_next = _firstChild;
        _firstChild = addThis;

        addThis->_prev = 0;
    }
    else {
        TIXMLASSERT( _lastChild == 0 );
        _firstChild = _lastChild = addThis;

        addThis->_prev = 0;
        addThis->_next = 0;
    }
    addThis->_parent = this;
    return addThis;
}


XMLNode* XMLNode::InsertAfterChild( XMLNode* afterThis, XMLNode* addThis )
{
	if (addThis->_document != _document)
		return 0;

    TIXMLASSERT( afterThis->_parent == this );

    if ( afterThis->_parent != this ) {
        return 0;
    }

    if ( afterThis->_next == 0 ) {
        // The last node or the only node.
        return InsertEndChild( addThis );
    }
	if (addThis->_parent)
		addThis->_parent->Unlink( addThis );
	else
	   addThis->_memPool->SetTracked();
    addThis->_prev = afterThis;
    addThis->_next = afterThis->_next;
    afterThis->_next->_prev = addThis;
    afterThis->_next = addThis;
    addThis->_parent = this;
    return addThis;
}




const XMLElement* XMLNode::FirstChildElement( const wchar_t* value ) const
{
    for( XMLNode* node=_firstChild; node; node=node->_next ) {
        XMLElement* element = node->ToElement();
        if ( element ) {
            if ( !value || XMLUtil::StringEqual( element->Name(), value ) ) {
                return element;
            }
        }
    }
    return 0;
}


const XMLElement* XMLNode::LastChildElement( const wchar_t* value ) const
{
    for( XMLNode* node=_lastChild; node; node=node->_prev ) {
        XMLElement* element = node->ToElement();
        if ( element ) {
            if ( !value || XMLUtil::StringEqual( element->Name(), value ) ) {
                return element;
            }
        }
    }
    return 0;
}


const XMLElement* XMLNode::NextSiblingElement( const wchar_t* value ) const
{
    for( XMLNode* node=this->_next; node; node = node->_next ) {
        const XMLElement* element = node->ToElement();
        if ( element
                && (!value || XMLUtil::StringEqual( value, node->Value() ))) {
            return element;
        }
    }
    return 0;
}


const XMLElement* XMLNode::PreviousSiblingElement( const wchar_t* value ) const
{
    for( XMLNode* node=_prev; node; node = node->_prev ) {
        const XMLElement* element = node->ToElement();
        if ( element
                && (!value || XMLUtil::StringEqual( value, node->Value() ))) {
            return element;
        }
    }
    return 0;
}


wchar_t* XMLNode::ParseDeep( wchar_t* p, StrPair* parentEnd )
{
    // This is a recursive method, but thinking about it "at the current level"
    // it is a pretty simple flat list:
    //		<foo/>
    //		<!-- comment -->
    //
    // With a special case:
    //		<foo>
    //		</foo>
    //		<!-- comment -->
    //
    // Where the closing element (/foo) *must* be the next thing after the opening
    // element, and the names must match. BUT the tricky bit is that the closing
    // element will be read by the child.
    //
    // 'endTag' is the end tag for this node, it is returned by a call to a child.
    // 'parentEnd' is the end tag for the parent, which is filled in and returned.

    while( p && *p ) {
        XMLNode* node = 0;

        p = _document->Identify( p, &node );
        if ( p == 0 || node == 0 ) {
            break;
        }

        StrPair endTag;
        p = node->ParseDeep( p, &endTag );
        if ( !p ) {
            DeleteNode( node );
            node = 0;
            if ( !_document->Error() ) {
                _document->SetError( XML_ERROR_PARSING, 0, 0 );
            }
            break;
        }

        XMLElement* ele = node->ToElement();
        // We read the end tag. Return it to the parent.
        if ( ele && ele->ClosingType() == XMLElement::CLOSING ) {
            if ( parentEnd ) {
                ele->_value.TransferTo( parentEnd );
            }
			node->_memPool->SetTracked();	// created and then immediately deleted.
            DeleteNode( node );
            return p;
        }

        // Handle an end tag returned to this level.
        // And handle a bunch of annoying errors.
        if ( ele ) {
            bool mismatch = false;
            if ( endTag.Empty() && ele->ClosingType() == XMLElement::OPEN ) {
                mismatch = true;
            }
            else if ( !endTag.Empty() && ele->ClosingType() != XMLElement::OPEN ) {
                mismatch = true;
            }
            else if ( !endTag.Empty() ) {
                if ( !XMLUtil::StringEqual( endTag.GetStr(), node->Value() )) {
                    mismatch = true;
                }
            }
            if ( mismatch ) {
                _document->SetError( XML_ERROR_MISMATCHED_ELEMENT, node->Value(), 0 );
                p = 0;
            }
        }
        if ( p == 0 ) {
            DeleteNode( node );
            node = 0;
        }
        if ( node ) {
            this->InsertEndChild( node );
        }
    }
    return 0;
}

void XMLNode::DeleteNode( XMLNode* node )
{
    if ( node == 0 ) {
        return;
    }
    MemPool* pool = node->_memPool;
    node->~XMLNode();
    pool->Free( node );
}

// --------- XMLText ---------- //
wchar_t* XMLText::ParseDeep( wchar_t* p, StrPair* )
{
    const wchar_t* start = p;
    if ( this->CData() ) {
        p = _value.ParseText( p, L"]]>", StrPair::NEEDS_NEWLINE_NORMALIZATION );
        if ( !p ) {
            _document->SetError( XML_ERROR_PARSING_CDATA, start, 0 );
        }
        return p;
    }
    else {
        int flags = _document->ProcessEntities() ? StrPair::TEXT_ELEMENT : StrPair::TEXT_ELEMENT_LEAVE_ENTITIES;
        if ( _document->WhitespaceMode() == COLLAPSE_WHITESPACE ) {
            flags |= StrPair::COLLAPSE_WHITESPACE;
        }

        p = _value.ParseText( p, L"<", flags );
        if ( !p ) {
            _document->SetError( XML_ERROR_PARSING_TEXT, start, 0 );
        }
        if ( p && *p ) {
            return p-1;
        }
    }
    return 0;
}


XMLNode* XMLText::ShallowClone( TiXMLDocument* doc ) const
{
    if ( !doc ) {
        doc = _document;
    }
    XMLText* text = doc->NewText( Value() );	// fixme: this will always allocate memory. Intern?
    text->SetCData( this->CData() );
    return text;
}


bool XMLText::ShallowEqual( const XMLNode* compare ) const
{
    const XMLText* text = compare->ToText();
    return ( text && XMLUtil::StringEqual( text->Value(), Value() ) );
}


bool XMLText::Accept( XMLVisitor* visitor ) const
{
    return visitor->Visit( *this );
}


// --------- XMLComment ---------- //

XMLComment::XMLComment( TiXMLDocument* doc ) : XMLNode( doc )
{
}


XMLComment::~XMLComment()
{
}


wchar_t* XMLComment::ParseDeep( wchar_t* p, StrPair* )
{
    // Comment parses as text.
    const wchar_t* start = p;
    p = _value.ParseText( p, L"-->", StrPair::COMMENT );
    if ( p == 0 ) {
        _document->SetError( XML_ERROR_PARSING_COMMENT, start, 0 );
    }
    return p;
}


XMLNode* XMLComment::ShallowClone( TiXMLDocument* doc ) const
{
    if ( !doc ) {
        doc = _document;
    }
    XMLComment* comment = doc->NewComment( Value() );	// fixme: this will always allocate memory. Intern?
    return comment;
}


bool XMLComment::ShallowEqual( const XMLNode* compare ) const
{
    const XMLComment* comment = compare->ToComment();
    return ( comment && XMLUtil::StringEqual( comment->Value(), Value() ));
}


bool XMLComment::Accept( XMLVisitor* visitor ) const
{
    return visitor->Visit( *this );
}


// --------- XMLDeclaration ---------- //

XMLDeclaration::XMLDeclaration( TiXMLDocument* doc ) : XMLNode( doc )
{
}


XMLDeclaration::~XMLDeclaration()
{
    //printf( "~XMLDeclaration\n" );
}


wchar_t* XMLDeclaration::ParseDeep( wchar_t* p, StrPair* )
{
    // Declaration parses as text.
    const wchar_t* start = p;
    p = _value.ParseText( p, L"?>", StrPair::NEEDS_NEWLINE_NORMALIZATION );
    if ( p == 0 ) {
        _document->SetError( XML_ERROR_PARSING_DECLARATION, start, 0 );
    }
    return p;
}


XMLNode* XMLDeclaration::ShallowClone( TiXMLDocument* doc ) const
{
    if ( !doc ) {
        doc = _document;
    }
    XMLDeclaration* dec = doc->NewDeclaration( Value() );	// fixme: this will always allocate memory. Intern?
    return dec;
}


bool XMLDeclaration::ShallowEqual( const XMLNode* compare ) const
{
    const XMLDeclaration* declaration = compare->ToDeclaration();
    return ( declaration && XMLUtil::StringEqual( declaration->Value(), Value() ));
}



bool XMLDeclaration::Accept( XMLVisitor* visitor ) const
{
    return visitor->Visit( *this );
}

// --------- XMLUnknown ---------- //

XMLUnknown::XMLUnknown( TiXMLDocument* doc ) : XMLNode( doc )
{
}


XMLUnknown::~XMLUnknown()
{
}


wchar_t* XMLUnknown::ParseDeep( wchar_t* p, StrPair* )
{
    // Unknown parses as text.
    const wchar_t* start = p;

    p = _value.ParseText( p, L">", StrPair::NEEDS_NEWLINE_NORMALIZATION );
    if ( !p ) {
        _document->SetError( XML_ERROR_PARSING_UNKNOWN, start, 0 );
    }
    return p;
}


XMLNode* XMLUnknown::ShallowClone( TiXMLDocument* doc ) const
{
    if ( !doc ) {
        doc = _document;
    }
    XMLUnknown* text = doc->NewUnknown( Value() );	// fixme: this will always allocate memory. Intern?
    return text;
}


bool XMLUnknown::ShallowEqual( const XMLNode* compare ) const
{
    const XMLUnknown* unknown = compare->ToUnknown();
    return ( unknown && XMLUtil::StringEqual( unknown->Value(), Value() ));
}


bool XMLUnknown::Accept( XMLVisitor* visitor ) const
{
    return visitor->Visit( *this );
}

// --------- XMLAttribute ---------- //

const wchar_t* XMLAttribute::Name() const 
{
    return _name.GetStr();
}

const wchar_t* XMLAttribute::Value() const 
{
    return _value.GetStr();
}

wchar_t* XMLAttribute::ParseDeep( wchar_t* p, bool processEntities )
{
    // Parse using the name rules: bug fix, was using ParseText before
    p = _name.ParseName( p );
    if ( !p || !*p ) {
        return 0;
    }

    // Skip white space before =
    p = XMLUtil::SkipWhiteSpace( p );
    if ( !p || *p != '=' ) {
        return 0;
    }

    ++p;	// move up to opening quote
    p = XMLUtil::SkipWhiteSpace( p );
    if ( *p != '\"' && *p != '\'' ) {
        return 0;
    }

    wchar_t endTag[2] = { *p, 0 };
    ++p;	// move past opening quote

    p = _value.ParseText( p, endTag, processEntities ? StrPair::ATTRIBUTE_VALUE : StrPair::ATTRIBUTE_VALUE_LEAVE_ENTITIES );
    return p;
}


void XMLAttribute::SetName( const wchar_t* n )
{
    _name.SetStr( n );
}


XMLError XMLAttribute::QueryIntValue( int* value ) const
{
    if ( XMLUtil::ToInt( Value(), value )) {
        return XML_NO_ERROR;
    }
    return XML_WRONG_ATTRIBUTE_TYPE;
}


XMLError XMLAttribute::QueryUnsignedValue( unsigned int* value ) const
{
    if ( XMLUtil::ToUnsigned( Value(), value )) {
        return XML_NO_ERROR;
    }
    return XML_WRONG_ATTRIBUTE_TYPE;
}


XMLError XMLAttribute::QueryBoolValue( bool* value ) const
{
    if ( XMLUtil::ToBool( Value(), value )) {
        return XML_NO_ERROR;
    }
    return XML_WRONG_ATTRIBUTE_TYPE;
}


XMLError XMLAttribute::QueryFloatValue( float* value ) const
{
    if ( XMLUtil::ToFloat( Value(), value )) {
        return XML_NO_ERROR;
    }
    return XML_WRONG_ATTRIBUTE_TYPE;
}


XMLError XMLAttribute::QueryDoubleValue( double* value ) const
{
    if ( XMLUtil::ToDouble( Value(), value )) {
        return XML_NO_ERROR;
    }
    return XML_WRONG_ATTRIBUTE_TYPE;
}


void XMLAttribute::SetAttribute( const wchar_t* v )
{
    _value.SetStr( v );
}


void XMLAttribute::SetAttribute( int v )
{
    wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    _value.SetStr( buf );
}


void XMLAttribute::SetAttribute( unsigned v )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    _value.SetStr( buf );
}


void XMLAttribute::SetAttribute( bool v )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    _value.SetStr( buf );
}

void XMLAttribute::SetAttribute( double v )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    _value.SetStr( buf );
}

void XMLAttribute::SetAttribute( float v )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    _value.SetStr( buf );
}

void XMLAttribute::SetAttribute(__int64 v)
{
	wchar_t buf[BUF_SIZE];
	XMLUtil::ToStr(v, buf, BUF_SIZE);
	_value.SetStr(buf);
}

// --------- XMLElement ---------- //
XMLElement::XMLElement( TiXMLDocument* doc ) : XMLNode( doc ),
    _closingType( 0 ),
    _rootAttribute( 0 )
{
}


XMLElement::~XMLElement()
{
    while( _rootAttribute ) {
        XMLAttribute* next = _rootAttribute->_next;
        DeleteAttribute( _rootAttribute );
        _rootAttribute = next;
    }
}


XMLAttribute* XMLElement::FindAttribute( const wchar_t* name )
{
    for( XMLAttribute* a = _rootAttribute; a; a = a->_next ) {
        if ( XMLUtil::StringEqual( a->Name(), name ) ) {
            return a;
        }
    }
    return 0;
}


const XMLAttribute* XMLElement::FindAttribute( const wchar_t* name ) const
{
    for( XMLAttribute* a = _rootAttribute; a; a = a->_next ) {
        if ( XMLUtil::StringEqual( a->Name(), name ) ) {
            return a;
        }
    }
    return 0;
}


const wchar_t* XMLElement::Attribute( const wchar_t* name, const wchar_t* value ) const
{
    const XMLAttribute* a = FindAttribute( name );
    if ( !a ) {
        return 0;
    }
    if ( !value || XMLUtil::StringEqual( a->Value(), value )) {
        return a->Value();
    }
    return 0;
}


const wchar_t* XMLElement::GetText() const
{
    if ( FirstChild() && FirstChild()->ToText() ) {
        return FirstChild()->Value();
    }
    return 0;
}


void	XMLElement::SetText( const wchar_t* inText )
{
	if ( FirstChild() && FirstChild()->ToText() )
		FirstChild()->SetValue( inText );
	else {
		XMLText*	theText = GetDocument()->NewText( inText );
		InsertFirstChild( theText );
	}
}


void XMLElement::SetText( int v ) 
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    SetText( buf );
}


void XMLElement::SetText( unsigned v ) 
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    SetText( buf );
}


void XMLElement::SetText( bool v ) 
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    SetText( buf );
}


void XMLElement::SetText( float v ) 
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    SetText( buf );
}


void XMLElement::SetText( double v ) 
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    SetText( buf );
}


XMLError XMLElement::QueryIntText( int* ival ) const
{
    if ( FirstChild() && FirstChild()->ToText() ) {
        const wchar_t* t = FirstChild()->Value();
        if ( XMLUtil::ToInt( t, ival ) ) {
            return XML_SUCCESS;
        }
        return XML_CAN_NOT_CONVERT_TEXT;
    }
    return XML_NO_TEXT_NODE;
}


XMLError XMLElement::QueryUnsignedText( unsigned* uval ) const
{
    if ( FirstChild() && FirstChild()->ToText() ) {
        const wchar_t* t = FirstChild()->Value();
        if ( XMLUtil::ToUnsigned( t, uval ) ) {
            return XML_SUCCESS;
        }
        return XML_CAN_NOT_CONVERT_TEXT;
    }
    return XML_NO_TEXT_NODE;
}


XMLError XMLElement::QueryBoolText( bool* bval ) const
{
    if ( FirstChild() && FirstChild()->ToText() ) {
        const wchar_t* t = FirstChild()->Value();
        if ( XMLUtil::ToBool( t, bval ) ) {
            return XML_SUCCESS;
        }
        return XML_CAN_NOT_CONVERT_TEXT;
    }
    return XML_NO_TEXT_NODE;
}


XMLError XMLElement::QueryDoubleText( double* dval ) const
{
    if ( FirstChild() && FirstChild()->ToText() ) {
        const wchar_t* t = FirstChild()->Value();
        if ( XMLUtil::ToDouble( t, dval ) ) {
            return XML_SUCCESS;
        }
        return XML_CAN_NOT_CONVERT_TEXT;
    }
    return XML_NO_TEXT_NODE;
}


XMLError XMLElement::QueryFloatText( float* fval ) const
{
    if ( FirstChild() && FirstChild()->ToText() ) {
        const wchar_t* t = FirstChild()->Value();
        if ( XMLUtil::ToFloat( t, fval ) ) {
            return XML_SUCCESS;
        }
        return XML_CAN_NOT_CONVERT_TEXT;
    }
    return XML_NO_TEXT_NODE;
}



XMLAttribute* XMLElement::FindOrCreateAttribute( const wchar_t* name )
{
    XMLAttribute* last = 0;
    XMLAttribute* attrib = 0;
    for( attrib = _rootAttribute;
            attrib;
            last = attrib, attrib = attrib->_next ) {
        if ( XMLUtil::StringEqual( attrib->Name(), name ) ) {
            break;
        }
    }
    if ( !attrib ) {
        attrib = new (_document->_attributePool.Alloc() ) XMLAttribute();
        attrib->_memPool = &_document->_attributePool;
        if ( last ) {
            last->_next = attrib;
        }
        else {
            _rootAttribute = attrib;
        }
        attrib->SetName( name );
        attrib->_memPool->SetTracked(); // always created and linked.
    }
    return attrib;
}


void XMLElement::DeleteAttribute( const wchar_t* name )
{
    XMLAttribute* prev = 0;
    for( XMLAttribute* a=_rootAttribute; a; a=a->_next ) {
        if ( XMLUtil::StringEqual( name, a->Name() ) ) {
            if ( prev ) {
                prev->_next = a->_next;
            }
            else {
                _rootAttribute = a->_next;
            }
            DeleteAttribute( a );
            break;
        }
        prev = a;
    }
}


wchar_t* XMLElement::ParseAttributes( wchar_t* p )
{
    const wchar_t* start = p;
    XMLAttribute* prevAttribute = 0;

    // Read the attributes.
    while( p ) {
        p = XMLUtil::SkipWhiteSpace( p );
        if ( !p || !(*p) ) {
            _document->SetError( XML_ERROR_PARSING_ELEMENT, start, Name() );
            return 0;
        }

        // attribute.
        if (XMLUtil::IsNameStartChar( *p ) ) {
            XMLAttribute* attrib = new (_document->_attributePool.Alloc() ) XMLAttribute();
            attrib->_memPool = &_document->_attributePool;
			attrib->_memPool->SetTracked();

            p = attrib->ParseDeep( p, _document->ProcessEntities() );
            if ( !p || Attribute( attrib->Name() ) ) {
                DeleteAttribute( attrib );
                _document->SetError( XML_ERROR_PARSING_ATTRIBUTE, start, p );
                return 0;
            }
            // There is a minor bug here: if the attribute in the source xml
            // document is duplicated, it will not be detected and the
            // attribute will be doubly added. However, tracking the 'prevAttribute'
            // avoids re-scanning the attribute list. Preferring performance for
            // now, may reconsider in the future.
            if ( prevAttribute ) {
                prevAttribute->_next = attrib;
            }
            else {
                _rootAttribute = attrib;
            }
            prevAttribute = attrib;
        }
        // end of the tag
        else if ( *p == '/' && *(p+1) == '>' ) {
            _closingType = CLOSED;
            return p+2;	// done; sealed element.
        }
        // end of the tag
        else if ( *p == '>' ) {
            ++p;
            break;
        }
        else {
            _document->SetError( XML_ERROR_PARSING_ELEMENT, start, p );
            return 0;
        }
    }
    return p;
}

void XMLElement::DeleteAttribute( XMLAttribute* attribute )
{
    if ( attribute == 0 ) {
        return;
    }
    MemPool* pool = attribute->_memPool;
    attribute->~XMLAttribute();
    pool->Free( attribute );
}

//
//	<ele></ele>
//	<ele>foo<b>bar</b></ele>
//
wchar_t* XMLElement::ParseDeep( wchar_t* p, StrPair* strPair )
{
    // Read the element name.
    p = XMLUtil::SkipWhiteSpace( p );
    if ( !p ) {
        return 0;
    }

    // The closing element is the </element> form. It is
    // parsed just like a regular element then deleted from
    // the DOM.
    if ( *p == '/' ) {
        _closingType = CLOSING;
        ++p;
    }

    p = _value.ParseName( p );
    if ( _value.Empty() ) {
        return 0;
    }

    p = ParseAttributes( p );
    if ( !p || !*p || _closingType ) {
        return p;
    }

    p = XMLNode::ParseDeep( p, strPair );
    return p;
}



XMLNode* XMLElement::ShallowClone( TiXMLDocument* doc ) const
{
    if ( !doc ) {
        doc = _document;
    }
    XMLElement* element = doc->NewElement( Value() );					// fixme: this will always allocate memory. Intern?
    for( const XMLAttribute* a=FirstAttribute(); a; a=a->Next() ) {
        element->SetAttribute( a->Name(), a->Value() );					// fixme: this will always allocate memory. Intern?
    }
    return element;
}


bool XMLElement::ShallowEqual( const XMLNode* compare ) const
{
    const XMLElement* other = compare->ToElement();
    if ( other && XMLUtil::StringEqual( other->Value(), Value() )) {

        const XMLAttribute* a=FirstAttribute();
        const XMLAttribute* b=other->FirstAttribute();

        while ( a && b ) {
            if ( !XMLUtil::StringEqual( a->Value(), b->Value() ) ) {
                return false;
            }
            a = a->Next();
            b = b->Next();
        }
        if ( a || b ) {
            // different count
            return false;
        }
        return true;
    }
    return false;
}


bool XMLElement::Accept( XMLVisitor* visitor ) const
{
    if ( visitor->VisitEnter( *this, _rootAttribute ) ) {
        for ( const XMLNode* node=FirstChild(); node; node=node->NextSibling() ) {
            if ( !node->Accept( visitor ) ) {
                break;
            }
        }
    }
    return visitor->VisitExit( *this );
}


// --------- TiXMLDocument ----------- //

// Warning: List must match 'enum XMLError'
const wchar_t* TiXMLDocument::_errorNames[XML_ERROR_COUNT] = {
    L"XML_SUCCESS",
    L"XML_NO_ATTRIBUTE",
    L"XML_WRONG_ATTRIBUTE_TYPE",
    L"XML_ERROR_FILE_NOT_FOUND",
    L"XML_ERROR_FILE_COULD_NOT_BE_OPENED",
    L"XML_ERROR_FILE_READ_ERROR",
    L"XML_ERROR_ELEMENT_MISMATCH",
    L"XML_ERROR_PARSING_ELEMENT",
    L"XML_ERROR_PARSING_ATTRIBUTE",
    L"XML_ERROR_IDENTIFYING_TAG",
    L"XML_ERROR_PARSING_TEXT",
    L"XML_ERROR_PARSING_CDATA",
    L"XML_ERROR_PARSING_COMMENT",
    L"XML_ERROR_PARSING_DECLARATION",
    L"XML_ERROR_PARSING_UNKNOWN",
    L"XML_ERROR_EMPTY_DOCUMENT",
    L"XML_ERROR_MISMATCHED_ELEMENT",
    L"XML_ERROR_PARSING",
    L"XML_CAN_NOT_CONVERT_TEXT",
    L"XML_NO_TEXT_NODE"
};


TiXMLDocument::TiXMLDocument( bool processEntities, Whitespace whitespace ) :
    XMLNode( 0 ),
    _writeBOM( false ),
    _processEntities( processEntities ),
    _errorID( XML_NO_ERROR ),
    _whitespace( whitespace ),
    _errorStr1( 0 ),
    _errorStr2( 0 ),
    _charBuffer( 0 )
{
    _document = this;	// avoid warning about 'this' in initializer list
}


TiXMLDocument::~TiXMLDocument()
{
    Clear();
}


void TiXMLDocument::Clear()
{
    DeleteChildren();

    _errorID = XML_NO_ERROR;
    _errorStr1 = 0;
    _errorStr2 = 0;

    delete [] _charBuffer;
    _charBuffer = 0;

#if 0
    _textPool.Trace( "text" );
    _elementPool.Trace( "element" );
    _commentPool.Trace( "comment" );
    _attributePool.Trace( "attribute" );
#endif
    
#ifdef DEBUG
    if ( Error() == false ) {
        TIXMLASSERT( _elementPool.CurrentAllocs()   == _elementPool.Untracked() );
        TIXMLASSERT( _attributePool.CurrentAllocs() == _attributePool.Untracked() );
        TIXMLASSERT( _textPool.CurrentAllocs()      == _textPool.Untracked() );
        TIXMLASSERT( _commentPool.CurrentAllocs()   == _commentPool.Untracked() );
    }
#endif
}


XMLElement* TiXMLDocument::NewElement( const wchar_t* name )
{
    XMLElement* ele = new (_elementPool.Alloc()) XMLElement( this );
    ele->_memPool = &_elementPool;
    ele->SetName( name );
    return ele;
}


XMLComment* TiXMLDocument::NewComment( const wchar_t* str )
{
    XMLComment* comment = new (_commentPool.Alloc()) XMLComment( this );
    comment->_memPool = &_commentPool;
    comment->SetValue( str );
    return comment;
}


XMLText* TiXMLDocument::NewText( const wchar_t* str )
{
    XMLText* text = new (_textPool.Alloc()) XMLText( this );
    text->_memPool = &_textPool;
    text->SetValue( str );
    return text;
}


XMLDeclaration* TiXMLDocument::NewDeclaration( const wchar_t* str )
{
    XMLDeclaration* dec = new (_commentPool.Alloc()) XMLDeclaration( this );
    dec->_memPool = &_commentPool;
    dec->SetValue( str ? str : L"xml version=\"1.0\" encoding=\"UTF-16\"" );
    return dec;
}


XMLUnknown* TiXMLDocument::NewUnknown( const wchar_t* str )
{
    XMLUnknown* unk = new (_commentPool.Alloc()) XMLUnknown( this );
    unk->_memPool = &_commentPool;
    unk->SetValue( str );
    return unk;
}

static FILE* callfopen( const wchar_t* filepath, const wchar_t* mode )
{
#if defined(_MSC_VER) && (_MSC_VER >= 1400 ) && (!defined WINCE)
    FILE* fp = 0;
	errno_t err = _wfopen_s(&fp, filepath, mode);
    if ( err ) {
        return 0;
    }
#else
    FILE* fp = fopen( filepath, mode );
#endif
    return fp;
}

XMLError TiXMLDocument::LoadFile( const wchar_t* filename )
{
    Clear();
    FILE* fp = callfopen( filename, L"rb" );
    if ( !fp ) {
        SetError( XML_ERROR_FILE_NOT_FOUND, filename, 0 );
        return _errorID;
    }
    LoadFile( fp );
    fclose( fp );
    return _errorID;
}


XMLError TiXMLDocument::LoadFile( FILE* fp )
{
    Clear();

    fseek( fp, 0, SEEK_SET );
    if ( fgetc( fp ) == EOF && ferror( fp ) != 0 ) {
        SetError( XML_ERROR_FILE_READ_ERROR, 0, 0 );
        return _errorID;
    }

    fseek( fp, 0, SEEK_END );
    const long filelength = ftell( fp );
    fseek( fp, 0, SEEK_SET );
    if ( filelength == -1L ) {
        SetError( XML_ERROR_FILE_READ_ERROR, 0, 0 );
        return _errorID;
    }

    const size_t size = filelength / sizeof(wchar_t);
    if ( size == 0 ) {
        SetError( XML_ERROR_EMPTY_DOCUMENT, 0, 0 );
        return _errorID;
    }

	_charBuffer = new wchar_t[size + sizeof(wchar_t)];
	size_t read = fread(_charBuffer, 1, filelength, fp);
	if (read != filelength) {
        SetError( XML_ERROR_FILE_READ_ERROR, 0, 0 );
        return _errorID;
    }

	_charBuffer[size] = L'\0';

    const wchar_t* p = _charBuffer;
    p = XMLUtil::SkipWhiteSpace( p );
    p = XMLUtil::ReadBOM( p, &_writeBOM );
    if ( !p || !*p ) {
        SetError( XML_ERROR_EMPTY_DOCUMENT, 0, 0 );
        return _errorID;
    }

	ptrdiff_t delta = p - _charBuffer;

    ParseDeep( _charBuffer + delta, 0 );
    return _errorID;
}


XMLError TiXMLDocument::SaveFile( const wchar_t* filename, bool compact )
{
    FILE* fp = callfopen( filename, L"wb, ccs=UNICODE" );
    if ( !fp ) {
        SetError( XML_ERROR_FILE_COULD_NOT_BE_OPENED, filename, 0 );
        return _errorID;
    }
    SaveFile(fp, compact);
    fclose( fp );
    return _errorID;
}


XMLError TiXMLDocument::SaveFile( FILE* fp, bool compact )
{
    XMLPrinter stream( fp, compact );
    Print( &stream );
    return _errorID;
}


XMLError TiXMLDocument::Parse( const wchar_t* p, size_t len )
{
	const wchar_t* start = p;
    Clear();

    if ( len == 0 || !p || !*p ) {
        SetError( XML_ERROR_EMPTY_DOCUMENT, 0, 0 );
        return _errorID;
    }
    if ( len == (size_t)(-1) ) {
        len = wcslen( p );
    }
	_charBuffer = new wchar_t[len + sizeof(wchar_t)];
	memset(_charBuffer, 0x00, len + sizeof(wchar_t));
	wmemcpy(_charBuffer, p, len);
	_charBuffer[len] = L'\0';

    p = XMLUtil::SkipWhiteSpace( p );
    p = XMLUtil::ReadBOM( p, &_writeBOM );
    if ( !p || !*p ) {
        SetError( XML_ERROR_EMPTY_DOCUMENT, 0, 0 );
        return _errorID;
    }

    ptrdiff_t delta = p - start;	// skip initial whitespace, BOM, etc.
    ParseDeep( _charBuffer+delta, 0 );
    if (_errorID) {
        // clean up now essentially dangling memory.
        // and the parse fail can put objects in the
        // pools that are dead and inaccessible.
        DeleteChildren();
        _elementPool.Clear();
        _attributePool.Clear();
        _textPool.Clear();
        _commentPool.Clear();
    }
    return _errorID;
}


void TiXMLDocument::Print( XMLPrinter* streamer ) const
{
    XMLPrinter stdStreamer( stdout );
    if ( !streamer ) {
        streamer = &stdStreamer;
    }
    Accept( streamer );
}


void TiXMLDocument::SetError( XMLError error, const wchar_t* str1, const wchar_t* str2 )
{
    TIXMLASSERT( error >= 0 && error < XML_ERROR_COUNT );
    _errorID = error;
    _errorStr1 = str1;
    _errorStr2 = str2;
}

const wchar_t* TiXMLDocument::ErrorName() const
{
	TIXMLASSERT( _errorID >= 0 && _errorID < XML_ERROR_COUNT );
	return _errorNames[_errorID];
}

void TiXMLDocument::PrintError() const
{
    if ( _errorID ) {
        static const int LEN = 20;
        wchar_t buf1[LEN] = { 0 };
		wchar_t buf2[LEN] = { 0 };

        if ( _errorStr1 ) {
            TIXML_SNPRINTF( buf1, LEN, L"%s", _errorStr1 );
        }
        if ( _errorStr2 ) {
            TIXML_SNPRINTF( buf2, LEN, L"%s", _errorStr2 );
        }

        wprintf( L"TiXMLDocument error id=%d '%s' str1=%s str2=%s\n",
                _errorID, ErrorName(), buf1, buf2 );
    }
}


XMLPrinter::XMLPrinter( FILE* file, bool compact, int depth ) :
    _elementJustOpened( false ),
    _firstElement( true ),
    _fp( file ),
    _depth( depth ),
    _textDepth( -1 ),
    _processEntities( true ),
    _compactMode( compact )
{
    for( int i=0; i<ENTITY_RANGE; ++i ) {
        _entityFlag[i] = false;
        _restrictedEntityFlag[i] = false;
    }
    for( int i=0; i<NUM_ENTITIES; ++i ) {
        TIXMLASSERT( entities[i].value < ENTITY_RANGE );
        if ( entities[i].value < ENTITY_RANGE ) {
            _entityFlag[ (int)entities[i].value ] = true;
        }
    }
    _restrictedEntityFlag[(int)'&'] = true;
    _restrictedEntityFlag[(int)'<'] = true;
    _restrictedEntityFlag[(int)'>'] = true;	// not required, but consistency is nice
    _buffer.Push( 0 );
}


void XMLPrinter::Print( const wchar_t* format, ... )
{
    va_list     va;
    va_start( va, format );

    if ( _fp ) {
		vfwprintf(_fp, format, va);
    }
    else {
#if defined(_MSC_VER) && (_MSC_VER >= 1400 )
		#if defined(WINCE)
		int len = 512;
		do {
		    len = len*2;
		    wchar_t* str = new char[len]();
			len = _vsnprintf(str, len, format, va);
			delete[] str;
		}while (len < 0);
		#else
		int len = _vscwprintf(format, va);
		#endif
#else
        int len = vsnprintf( 0, 0, format, va );
#endif
        // Close out and re-start the va-args
        va_end( va );
        va_start( va, format );
        wchar_t* p = _buffer.PushArr( len ) - 1;	// back up over the null terminator.
#if defined(_MSC_VER) && (_MSC_VER >= 1400 )
		#if defined(WINCE)
		_vsnprintf( p, len+1, format, va );
		#else
		_vsnwprintf_s(p, len + 1, _TRUNCATE, format, va);
		#endif
#else
		vsnprintf( p, len+1, format, va );
#endif
    }
    va_end( va );
}


void XMLPrinter::PrintSpace( int depth )
{
    for( int i=0; i<depth; ++i ) {
        Print( L"    " );
    }
}


void XMLPrinter::PrintString( const wchar_t* p, bool restricted )
{
    // Look for runs of bytes between entities to print.
    const wchar_t* q = p;
    const bool* flag = restricted ? _restrictedEntityFlag : _entityFlag;

    if ( _processEntities ) {
        while ( *q ) {
            // Remember, char is sometimes signed. (How many times has that bitten me?)
            if ( *q > 0 && *q < ENTITY_RANGE ) {
                // Check for entities. If one is found, flush
                // the stream up until the entity, write the
                // entity, and keep looking.
                if ( flag[(unsigned)(*q)] ) {
                    while ( p < q ) {
                        Print( L"%c", *p );
                        ++p;
                    }
                    for( int i=0; i<NUM_ENTITIES; ++i ) {
                        if ( entities[i].value == *q ) {
                            Print( L"&%s;", entities[i].pattern );
                            break;
                        }
                    }
                    ++p;
                }
            }
            ++q;
        }
    }
    // Flush the remaining string. This will be the entire
    // string if an entity wasn't found.
    if ( !_processEntities || (q-p > 0) ) {
        Print( L"%s", p );
    }
}


void XMLPrinter::PushHeader( bool writeBOM, bool writeDec )
{
    if ( writeBOM ) {
        static const unsigned char bom[] = { TIXML_UTF_LEAD_0, TIXML_UTF_LEAD_1, TIXML_UTF_LEAD_2, 0 };
        Print( L"%s", bom );
    }
    if ( writeDec ) {
        PushDeclaration( L"xml version=\"1.0\"" );
    }
}


void XMLPrinter::OpenElement( const wchar_t* name, bool compactMode )
{
    if ( _elementJustOpened ) {
        SealElement();
    }
    _stack.Push( name );

    if ( _textDepth < 0 && !_firstElement && !compactMode ) {
        Print( L"\n" );
    }
    if ( !compactMode ) {
        PrintSpace( _depth );
    }

    Print( L"<%s", name );
    _elementJustOpened = true;
    _firstElement = false;
    ++_depth;
}


void XMLPrinter::PushAttribute( const wchar_t* name, const wchar_t* value )
{
    TIXMLASSERT( _elementJustOpened );
    Print( L" %s=\"", name );
    PrintString( value, false );
    Print( L"\"" );
}


void XMLPrinter::PushAttribute( const wchar_t* name, int v )
{
    wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    PushAttribute( name, buf );
}


void XMLPrinter::PushAttribute( const wchar_t* name, unsigned v )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    PushAttribute( name, buf );
}


void XMLPrinter::PushAttribute( const wchar_t* name, bool v )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    PushAttribute( name, buf );
}


void XMLPrinter::PushAttribute( const wchar_t* name, double v )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( v, buf, BUF_SIZE );
    PushAttribute( name, buf );
}


void XMLPrinter::CloseElement( bool compactMode )
{
    --_depth;
    const wchar_t* name = _stack.Pop();

    if ( _elementJustOpened ) {
        Print( L"/>" );
    }
    else {
        if ( _textDepth < 0 && !compactMode) {
            Print( L"\n" );
            PrintSpace( _depth );
        }
        Print( L"</%s>", name );
    }

    if ( _textDepth == _depth ) {
        _textDepth = -1;
    }
    if ( _depth == 0 && !compactMode) {
        Print( L"\n" );
    }
    _elementJustOpened = false;
}


void XMLPrinter::SealElement()
{
    _elementJustOpened = false;
    Print( L">" );
}


void XMLPrinter::PushText( const wchar_t* text, bool cdata )
{
    _textDepth = _depth-1;

    if ( _elementJustOpened ) {
        SealElement();
    }
    if ( cdata ) {
        Print( L"<![CDATA[" );
        Print( L"%s", text );
        Print( L"]]>" );
    }
    else {
        PrintString( text, true );
    }
}

void XMLPrinter::PushText( int value )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( value, buf, BUF_SIZE );
    PushText( buf, false );
}


void XMLPrinter::PushText( unsigned value )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( value, buf, BUF_SIZE );
    PushText( buf, false );
}


void XMLPrinter::PushText( bool value )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( value, buf, BUF_SIZE );
    PushText( buf, false );
}


void XMLPrinter::PushText( float value )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( value, buf, BUF_SIZE );
    PushText( buf, false );
}


void XMLPrinter::PushText( double value )
{
	wchar_t buf[BUF_SIZE];
    XMLUtil::ToStr( value, buf, BUF_SIZE );
    PushText( buf, false );
}


void XMLPrinter::PushComment( const wchar_t* comment )
{
    if ( _elementJustOpened ) {
        SealElement();
    }
    if ( _textDepth < 0 && !_firstElement && !_compactMode) {
        Print( L"\n" );
        PrintSpace( _depth );
    }
    _firstElement = false;
    Print( L"<!--%s-->", comment );
}


void XMLPrinter::PushDeclaration( const wchar_t* value )
{
    if ( _elementJustOpened ) {
        SealElement();
    }
    if ( _textDepth < 0 && !_firstElement && !_compactMode) {
        Print( L"\n" );
        PrintSpace( _depth );
    }
    _firstElement = false;
    Print( L"<?%s?>", value );
}


void XMLPrinter::PushUnknown( const wchar_t* value )
{
    if ( _elementJustOpened ) {
        SealElement();
    }
    if ( _textDepth < 0 && !_firstElement && !_compactMode) {
        Print( L"\n" );
        PrintSpace( _depth );
    }
    _firstElement = false;
    Print( L"<!%s>", value );
}


bool XMLPrinter::VisitEnter( const TiXMLDocument& doc )
{
    _processEntities = doc.ProcessEntities();
    if ( doc.HasBOM() ) {
        PushHeader( true, false );
    }
    return true;
}


bool XMLPrinter::VisitEnter( const XMLElement& element, const XMLAttribute* attribute )
{
	const XMLElement*	parentElem = element.Parent()->ToElement();
	bool		compactMode = parentElem ? CompactMode(*parentElem) : _compactMode;
    OpenElement( element.Name(), compactMode );
    while ( attribute ) {
        PushAttribute( attribute->Name(), attribute->Value() );
        attribute = attribute->Next();
    }
    return true;
}


bool XMLPrinter::VisitExit( const XMLElement& element )
{
    CloseElement( CompactMode(element) );
    return true;
}


bool XMLPrinter::Visit( const XMLText& text )
{
    PushText( text.Value(), text.CData() );
    return true;
}


bool XMLPrinter::Visit( const XMLComment& comment )
{
    PushComment( comment.Value() );
    return true;
}

bool XMLPrinter::Visit( const XMLDeclaration& declaration )
{
    PushDeclaration( declaration.Value() );
    return true;
}


bool XMLPrinter::Visit( const XMLUnknown& unknown )
{
    PushUnknown( unknown.Value() );
    return true;
}

}   // namespace tinyxml2

