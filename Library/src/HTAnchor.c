/*								     HTAnchor.c
**	HYPERTEXT "ANCHOR" OBJECT
**
**	(c) COPYRIGHT MIT 1995.
**	Please first read the full copyright statement in the file COPYRIGH.
**
**	An anchor represents a region of a hypertext document which is
**	linked to another anchor in the same or a different document.
**
** History
**         Nov 1990  Written in Objective-C for the NeXT browser (TBL)
**	24-Oct-1991 (JFG), written in C, browser-independant 
**	21-Nov-1991 (JFG), first complete version
**	 3-May-1995 (HF), Added a lot of methods and other stuff
*/

/* Library include files */
#include "tcp.h"
#include "HTUtils.h"
#include "HTString.h"
#include "HTFormat.h"
#include "HTParse.h"
#include "HTMethod.h"
#include "HTFWrite.h"					  /* for cache stuff */
#include "HTAnchor.h"					 /* Implemented here */

#define HASH_SIZE 101		/* Arbitrary prime. Memory/speed tradeoff */

typedef struct _HyperDoc Hyperdoc;
#ifdef VMS
struct _HyperDoc {
	int junk;	/* VMS cannot handle pointers to undefined structs */
};
#endif

PRIVATE HTList **adult_table=0;  /* Point to table of lists of all parents */

/* ------------------------------------------------------------------------- */
/*				Creation Methods			     */
/* ------------------------------------------------------------------------- */

/*
**	Do not use "new" by itself outside this module. In order to enforce
**	consistency, we insist that you furnish more information about the
**	anchor you are creating : use newWithParent or newWithAddress.
*/
PRIVATE HTParentAnchor * HTParentAnchor_new (void)
{
    HTParentAnchor *newAnchor =
	(HTParentAnchor *) calloc(1, sizeof (HTParentAnchor));
    if (!newAnchor) outofmem(__FILE__, "HTParentAnchor_new");
    newAnchor->parent = newAnchor;
    newAnchor->content_type = WWW_UNKNOWN;
    newAnchor->mainLink.method = METHOD_INVALID;
    newAnchor->date = (time_t) -1;
    newAnchor->expires = (time_t) -1;
    newAnchor->last_modified = (time_t) -1;
    return newAnchor;
}


PRIVATE HTChildAnchor * HTChildAnchor_new (void)
{
    HTChildAnchor *child = (HTChildAnchor *) calloc (1, sizeof(HTChildAnchor));
    if (!child) outofmem(__FILE__, "HTChildAnchor_new");
    return child;
}


/*	Create new or find old child anchor
**	-----------------------------------
**
**	Me one is for a new anchor being edited into an existing
**	document. The parent anchor must already exist. All
**	children without tags (no NAME attribut) points to the same NULL
**	child.
*/
PUBLIC HTChildAnchor * HTAnchor_findChild (HTParentAnchor *	parent,
					   CONST char *		tag)
{
    HTChildAnchor *child;
    HTList *kids;
    
    if (!parent) {
	if (ANCH_TRACE)
	    TTYPrint(TDEST, "Find Child.. called with NULL parent.\n");
	return NULL;
    }

    /* First search list of children to see if tag is already there */
    if ((kids = parent->children)) {
	if (tag && *tag) {	/* TBL */
	    while ((child = (HTChildAnchor *) HTList_nextObject(kids))) {
		if (child->tag && !strcmp(child->tag, tag)) {
		    if (ANCH_TRACE)
			TTYPrint(TDEST,
				 "Find Child.. %p of parent %p with name `%s' already exists.\n",
				 (void *) child, (void *) parent, tag);
		    return child;
		}
	    }
	}
    } else				      /* Create new list of children */
	parent->children = HTList_new ();
    
    /* Did't find it so we need to create a new one */
    child = HTChildAnchor_new();
    HTList_addObject(parent->children, child);
    child->parent = parent;
    StrAllocCopy(child->tag, tag);
    if (ANCH_TRACE)
	TTYPrint(TDEST,"Find Child.. New Anchor %p named `%s' is child of %p\n",
		(void *) child, tag ? tag : (CONST char *) "", (void *)parent);
    return child;
}


/*	Create new or find old named anchor
**	-----------------------------------
**
**	Me one is for a reference which is found in a document, and might
**	not be already loaded.
**	Note: You are not guaranteed a new anchor -- you might get an old one,
**	like with fonts.
*/
PUBLIC HTAnchor * HTAnchor_findAddress (CONST char * address)
{
    char *tag = HTParse (address, "", PARSE_ANCHOR);	        /* Any tags? */
    
    /* If the address represents a sub-anchor, we recursively load its parent,
       then we create a child anchor within that document. */
    if (*tag) {
	char *addr = HTParse(address, "", PARSE_ACCESS | PARSE_HOST |
			     PARSE_PATH | PARSE_PUNCTUATION);
	HTParentAnchor * parent = (HTParentAnchor*) HTAnchor_findAddress(addr);
	HTChildAnchor * child = HTAnchor_findChild(parent, tag);
	free(addr);
	free(tag);
	return (HTAnchor *) child;
    } else {		       	     /* Else check whether we have this node */
	int hash;
	CONST char *p;
	HTList * adults;
	HTList *grownups;
	HTParentAnchor * foundAnchor;
	char *newaddr = NULL;
	StrAllocCopy(newaddr, address);		         /* Get our own copy */
	free(tag);
	newaddr = HTSimplify(&newaddr);

	/* Select list from hash table */
	for(p=newaddr, hash=0; *p; p++)
	    hash = (int) ((hash * 3 + (*(unsigned char*)p)) % HASH_SIZE);
	if (!adult_table) {
	    adult_table = (HTList**) calloc(HASH_SIZE, sizeof(HTList*));
	    if (!adult_table) outofmem(__FILE__, "HTAnchor_findAddress");
	}
	if (!adult_table[hash]) adult_table[hash] = HTList_new();
	adults = adult_table[hash];

	/* Search list for anchor */
	grownups = adults;
	while ((foundAnchor = (HTParentAnchor *) HTList_nextObject(grownups))){
	    if (!strcmp(foundAnchor->address, newaddr)) {
		if (ANCH_TRACE)
		    TTYPrint(TDEST, "Find Parent. %p with address `%s' already exists.\n",
			    (void*) foundAnchor, newaddr);
		free(newaddr);			       /* We already have it */
		return (HTAnchor *) foundAnchor;
	    }
	}
	
	/* Node not found : create new anchor. */
	foundAnchor = HTParentAnchor_new();
	foundAnchor->address = newaddr;			/* Remember our copy */
	HTList_addObject (adults, foundAnchor);
	if (ANCH_TRACE) TTYPrint(TDEST, "Find Parent. %p with hash %d and address `%s' created\n", (void*)foundAnchor, hash, newaddr);
	return (HTAnchor *) foundAnchor;
    }
}

/* ------------------------------------------------------------------------- */
/*				   Link Methods				     */
/* ------------------------------------------------------------------------- */

/*	Create or find a child anchor with a possible link
**	--------------------------------------------------
**
**	Create new anchor with a given parent and possibly
**	a name, and possibly a link to a _relatively_ named anchor.
**	All parameters EXCEPT parent can be NULL
*/
PUBLIC HTChildAnchor * HTAnchor_findChildAndLink (HTParentAnchor *	parent,
						  CONST char *		tag,
						  CONST char *		href,
						  HTLinkType *		ltype)
{
    HTChildAnchor * child = HTAnchor_findChild(parent, tag);
    if (href && *href) {
	char * relative_to = HTAnchor_address((HTAnchor *) parent);
	char * parsed_address = HTParse(href, relative_to, PARSE_ALL);
	HTAnchor * dest = HTAnchor_findAddress(parsed_address);
	HTAnchor_link((HTAnchor *) child, dest, ltype, METHOD_INVALID);
	free(parsed_address);
	free(relative_to);
    }
    return child;
}

/*	Link me Anchor to another given one
**	-------------------------------------
*/
PUBLIC BOOL HTAnchor_link (HTAnchor *	source,
			   HTAnchor * 	destination, 
			   HTLinkType *	type,
			   HTMethod	method)
{
    if (!(source && destination))
	return NO;		/* Can't link to/from non-existing anchor */
    if (ANCH_TRACE)
	TTYPrint(TDEST, "Link Anchors anchor %p to anchor %p\n",
		(void *) source, (void *) destination);
    if (!source->mainLink.dest) {
	source->mainLink.dest = destination;
	source->mainLink.type = type;
	source->mainLink.method = method;
    } else {
	HTLink * newLink = (HTLink *) calloc(1, sizeof (HTLink));
	if (!newLink) outofmem(__FILE__, "HTAnchor_link");
	newLink->dest = destination;
	newLink->type = type;
	newLink->method = method;
	if (!source->links)
	    source->links = HTList_new();
	HTList_addObject (source->links, newLink);
    }
    if (!destination->parent->sources)
	destination->parent->sources = HTList_new ();
    HTList_addObject (destination->parent->sources, source);
    return YES;
}


/*
**  Find the anchor object between a destination and a source ancher.
**  Return link object if any, else NULL
*/
PUBLIC HTLink *HTAnchor_findLink (HTAnchor * src, HTAnchor * dest)
{
    if (src && dest) {
	if (src->mainLink.dest == dest)
	    return &(src->mainLink);
	if (src->links) {
	    HTList *cur = src->links;
	    HTLink *pres;
	    while ((pres = (HTLink *) HTList_nextObject(cur)) != NULL) {
		if (pres->dest == dest)
		    return pres;
	    }
	}
    }
    return NULL;
}


/*  HTAnchor_setLink
**  ----------------
**  When a link has been used for posting an object from a source to a
**  destination link, the result of the operation is stored as part of the
**  link information.
*/
PUBLIC BOOL HTAnchor_setLinkResult (HTLink * link, HTLinkResult result)
{
    if (link) {
	link->result = result;
	return YES;
    }
    return NO;
}

/*
**  Returns the main destination of this anchor
*/
PUBLIC HTAnchor * HTAnchor_followMainLink (HTAnchor * me)
{
    return me ? me->mainLink.dest : NULL;
}


/*
**  Returns the methods registered for the main destination of this
**  anchor
*/
PUBLIC HTMethod HTAnchor_mainLinkMethod (HTAnchor * me)
{
    return me ? me->mainLink.method : METHOD_INVALID;
}


/*
**  Moves all link information from one anchor to another.
**  This is used in redirection etc.
**  Returns YES if OK, else NO
*/
PUBLIC BOOL HTAnchor_moveAllLinks (HTAnchor * src, HTAnchor * dest)
{
    if (!src || !dest) return NO;
    if (ANCH_TRACE)
	TTYPrint(TDEST, "Move Links.. from anchor %p to anchor %p\n",
		(void *) src, (void *) dest);

    /* Move main link information */
    dest->mainLink.dest = src->mainLink.dest;
    dest->mainLink.type = src->mainLink.type;
    dest->mainLink.method = src->mainLink.method;
    dest->mainLink.result = src->mainLink.result;

    src->mainLink.dest = NULL;
    src->mainLink.type = NULL;
    src->mainLink.method = METHOD_INVALID;
    src->mainLink.result = HT_LINK_INVALID;

    /* Move link information for other links */
    if (dest->links) {
	HTList *cur = dest->links;
	HTLink *pres;
	while ((pres = (HTLink *) HTList_nextObject(cur)))
	    free(pres);
	HTList_delete(dest->links);
    }
    dest->links = src->links;
    src->links = NULL;
    return YES;
}


/*
**  Removes all link information from one anchor to another.
**  Returns YES if OK, else NO
*/
PUBLIC BOOL HTAnchor_removeLink (HTAnchor * src, HTAnchor * dest)
{
    if (!src || !dest) return NO;
    if (ANCH_TRACE)
	TTYPrint(TDEST, "Remove Link. from anchor %p to anchor %p\n",
		(void *) src, (void *) dest);

    /* Remove if dest is the main link */
    if (src->mainLink.dest == dest) {
	src->mainLink.dest = NULL;
	src->mainLink.type = NULL;
	src->mainLink.method = METHOD_INVALID;
	src->mainLink.result = HT_LINK_INVALID;
	return YES;
    }

    /* Remove link information for other links */
    if (dest->links) {
	HTList *cur = dest->links;
	HTLink *pres;
	while ((pres = (HTLink *) HTList_nextObject(cur))) {
	    if (pres->dest == dest) {
		HTList_removeObject(dest->links, pres);
		free(pres);
		return YES;
	    }
	}
    }
    return NO;
}

/*
**  Removes all link information
**  Returns YES if OK, else NO
*/
PUBLIC BOOL HTAnchor_removeAllLinks (HTAnchor * me)
{
    if (!me) return NO;
    if (ANCH_TRACE)
	TTYPrint(TDEST, "Remove Link. from anchor %p\n", (void *) me);

    /* Remove if dest is the main link */
    me->mainLink.dest = NULL;
    me->mainLink.type = NULL;
    me->mainLink.method = METHOD_INVALID;
    me->mainLink.result = HT_LINK_INVALID;

    /* Remove link information for other links */
    if (me->links) {
	HTList *cur = me->links;
	HTLink *pres;
	while ((pres = (HTLink *) HTList_nextObject(cur)))
	    free(pres);
	HTList_delete(me->links);
	me->links = NULL;
    }
    return YES;
}

/*
**  Returns a link with a given link type or NULL if nothing found
*/
PUBLIC HTAnchor * HTAnchor_followTypedLink (HTAnchor * me, HTLinkType * type)
{
    if (me->mainLink.type == type)
	return me->mainLink.dest;
    if (me->links) {
	HTList *links = me->links;
	HTLink *link;
	while ((link = (HTLink *) HTList_nextObject (links)))
	    if (link->type == type)
		return link->dest;
    }
    return NULL;		/* No link of me type */
}


/*
**  Upgrade the link to the main destination and and downgrade the
**  current main link to the list
*/
PUBLIC BOOL HTAnchor_makeMainLink  (HTAnchor * me, HTLink * movingLink)
{
    if (!(me && me->links && movingLink &&
	  HTList_removeObject(me->links, movingLink)))
	return NO;
    else {
	/* First push current main link onto top of links list */
	HTLink *newLink = (HTLink*) malloc (sizeof (HTLink));
	if (newLink == NULL) outofmem(__FILE__, "HTAnchor_makeMainLink");
	memcpy (newLink, & me->mainLink, sizeof (HTLink));
	HTList_addObject (me->links, newLink);

	/* Now make movingLink the new main link, and free it */
	memcpy (& me->mainLink, movingLink, sizeof (HTLink));
	free (movingLink);
	return YES;
    }
}

/*		Move an anchor to the head of the list of its siblings
**		------------------------------------------------------
**
**	This is to ensure that an anchor which might have already existed
**	is put in the correct order as we load the document.
*/
PUBLIC void HTAnchor_makeLastChild (HTChildAnchor * me)
{
    if (me->parent != (HTParentAnchor *) me) { /* Make sure it's a child */
	HTList * siblings = me->parent->children;
	HTList_removeObject (siblings, me);
	HTList_addObject (siblings, me);
    }
}

/* ------------------------------------------------------------------------- */
/*				Deletion Methods			     */
/* ------------------------------------------------------------------------- */

/*	Delete an anchor and possibly related things (auto garbage collection)
**	--------------------------------------------
**
**	The anchor is only deleted if the corresponding document is not loaded.
**	All outgoing links from parent and children are deleted, and this
**	anchor is removed from the sources list of all its targets.
**	We also try to delete the targets whose documents are not loaded.
**	If this anchor's source list is empty, we delete it and its children.
*/

/*	Deletes all the memory allocated in a parent anchor and returns the
**	hyperdoc
*/
PRIVATE HyperDoc * delete_parent (HTParentAnchor * me)
{
    HyperDoc *doc = me->document;

    /* Remove link and address information */
    if (me->links) {
	HTList *cur = me->links;
	HTLink *pres;
	while ((pres = (HTLink *) HTList_nextObject(cur)))
	    free(pres);
	HTList_delete(me->links);
    }
    HTList_delete (me->children);
    HTList_delete (me->sources);
    FREE(me->physical);
    FREE(me->address);

    /* Then remove entity header information (metainformation) */
    FREE(me->title);
    FREE(me->derived_from);
    FREE(me->version);
    if (me->extra_headers) {
	HTList *cur = me->extra_headers;
	char *pres;
	while ((pres = (char *) HTList_nextObject(cur)))
	    free(pres);
	HTList_delete(me->extra_headers);
    }
    free(me);
    return doc;
}


/*	Delete a parent anchor and all its children. If a HyperDoc is found
**	then it's returned
*/
PRIVATE HyperDoc *delete_family (HTAnchor * me)
{
    HTParentAnchor *parent = me->parent;
    if (ANCH_TRACE)
	TTYPrint(TDEST, "AnchorDelete Remove parent %p and children\n", parent);
    if (!me) {
	if (ANCH_TRACE)
	    TTYPrint(TDEST, "AnchorDelete No anchor found\n");
	return NULL;
    }

    /* Delete children */
    if (parent->children) {
	HTChildAnchor *child;
	while ((child = (HTChildAnchor *)
		HTList_removeLastObject(parent->children))) {
	    FREE(child->tag);
	    if (child->links) {
		HTList *cur = child->links;
		HTLink *pres;
		while ((pres = (HTLink *) HTList_nextObject(cur)))
		    free(pres);
		HTList_delete(child->links);
	    }
	    free(child);
	}
    }
    return delete_parent(parent);
}


/*	DELETE ALL ANCHORS
**	------------------
**	Deletes all anchors and return a list of all the HyperDocs found.
**	It is for the application to delete any HyperDocs.
**	If NULL then no hyperdocs are returned
**	Return YES if OK, else NO
*/
PUBLIC BOOL HTAnchor_deleteAll (HTList * documents)
{
    int cnt;
    HTList *cur;
    if (!adult_table)
	return NO;
    for (cnt=0; cnt<HASH_SIZE; cnt++) {
	if ((cur = adult_table[cnt])) { 
	    HTParentAnchor *pres;
	    while ((pres = (HTParentAnchor *) HTList_nextObject(cur)) != NULL){
		HyperDoc *doc = delete_family((HTAnchor *) pres);
		if (doc && documents) HTList_addObject(documents, (void *)doc);
	    }
	}
	HTList_delete(adult_table[cnt]);
    }
    FREE(adult_table);
    return YES;
}


PRIVATE void deleteLinks (HTAnchor * me)
{
  if (! me)
    return;

  /* Recursively try to delete target anchors */
  if (me->mainLink.dest) {
    HTParentAnchor *parent = me->mainLink.dest->parent;
    HTList_removeObject (parent->sources, me);
    if (! parent->document)  /* Test here to avoid calling overhead */
      HTAnchor_delete (parent);
  }
  if (me->links) {  /* Extra destinations */
    HTLink *target;
    while ((target = (HTLink *) HTList_removeLastObject (me->links))) {
      HTParentAnchor *parent = target->dest->parent;
      HTList_removeObject (parent->sources, me);
      if (! parent->document)  /* Test here to avoid calling overhead */
	HTAnchor_delete (parent);
    }
  }
}

PUBLIC BOOL HTAnchor_delete (HTParentAnchor * me)
{
  HTChildAnchor *child;

  /* Don't delete if document is loaded */
  if (me->document)
    return NO;

  /* Recursively try to delete target anchors */
  deleteLinks ((HTAnchor *) me);

  if (! HTList_isEmpty (me->sources)) {  /* There are still incoming links */
    /* Delete all outgoing links from children, if any */
    HTList *kids = me->children;
    while ((child = (HTChildAnchor *) HTList_nextObject (kids)))
      deleteLinks ((HTAnchor *) child);
    return NO;  /* Parent not deleted */
  }

  /* No more incoming links : kill everything */
  /* First, recursively delete children */
  while ((child = (HTChildAnchor *) HTList_removeLastObject (me->children))) {
    deleteLinks ((HTAnchor *) child);
    free (child->tag);
    free (child);
  }

  /* Now kill myself */
  delete_parent(me);
  return YES;  /* Parent deleted */
}

/* ------------------------------------------------------------------------- */
/*				Data Access Methods			     */
/* ------------------------------------------------------------------------- */

PUBLIC HTParentAnchor * HTAnchor_parent  (HTAnchor * me)
{
    return me ? me->parent : NULL;
}

PUBLIC void HTAnchor_setDocument  (HTParentAnchor * me, HyperDoc * doc)
{
    if (me)
	me->document = doc;
}

PUBLIC HyperDoc * HTAnchor_document  (HTParentAnchor * me)
{
    return me ? me->document : NULL;
}


#if 0
/* We might want to use this when we have a link editing application */
PUBLIC void HTAnchor_setAddress
   (HTAnchor * me, char * addr)
{
  if (me)
    StrAllocCopy (me->parent->address, addr);
}
#endif

PUBLIC char * HTAnchor_address  (HTAnchor * me)
{
    char *addr = NULL;
    if (me) {
	if (((HTParentAnchor *) me == me->parent) ||
	    !((HTChildAnchor *) me)->tag) { /* it's an adult or no tag */
	    StrAllocCopy (addr, me->parent->address);
	}
	else {			/* it's a named child */
	    addr = (char *) malloc (2 + strlen (me->parent->address)
				    + strlen (((HTChildAnchor *) me)->tag));
	    if (addr == NULL) outofmem(__FILE__, "HTAnchor_address");
	    sprintf (addr, "%s#%s", me->parent->address,
		     ((HTChildAnchor *) me)->tag);
	}
    }
    return addr;
}

PUBLIC BOOL HTAnchor_hasChildren  (HTParentAnchor * me)
{
    return me ? ! HTList_isEmpty(me->children) : NO;
}

PUBLIC void HTAnchor_clearIndex  (HTParentAnchor * me)
{
    if (me) me->isIndex = NO;
}

PUBLIC void HTAnchor_setIndex  (HTParentAnchor * me)
{
  if (me) me->isIndex = YES;
}

PUBLIC BOOL HTAnchor_isIndex  (HTParentAnchor * me)
{
    return me ? me->isIndex : NO;
}

/*	Protocol
**	--------
*/

PUBLIC void * HTAnchor_protocol (HTParentAnchor * me)
{
    return me ? me->protocol : NULL;
}

PUBLIC void HTAnchor_setProtocol (HTParentAnchor * me,
	void* 	protocol)
{
    if (me) me->protocol = protocol;
}

/*	Physical Address
**	----------------
*/

PUBLIC char * HTAnchor_physical (HTParentAnchor * me)
{
    return me ? me->physical : NULL;
}

PUBLIC void HTAnchor_setPhysical (HTParentAnchor * me,
	char * 	physical)
{
    if (!me || !physical) {
	if (ANCH_TRACE)
	    TTYPrint(TDEST, "HTAnchor.... setPhysical, called with null argument\n");
	return;
    }
    StrAllocCopy(me->physical, physical);
}

/*	Cache Information
**	-----------------
*/
PUBLIC BOOL HTAnchor_cacheHit (HTParentAnchor * me)
{
    return me ? me->cacheHit : NO;
}

PUBLIC void HTAnchor_setCacheHit (HTParentAnchor * me, BOOL cacheHit)
{
    if (me) me->cacheHit = cacheHit;
}

/* ------------------------------------------------------------------------- */
/*			      Entity Header Information			     */
/* ------------------------------------------------------------------------- */

/*
**	Media Types (Content-Type)
*/
PUBLIC HTFormat HTAnchor_format (HTParentAnchor * me)
{
    return me ? me->content_type : NULL;
}

PUBLIC void HTAnchor_setFormat (HTParentAnchor * me, HTFormat form)
{
    if (me) me->content_type = form;
}

/*
**	Charset parameter to Content-Type
*/
PUBLIC HTCharset HTAnchor_charset (HTParentAnchor * me)
{
    return me ? me->charset : NULL;
}

PUBLIC void HTAnchor_setCharset (HTParentAnchor * me, HTCharset charset)
{
    if (me) me->charset = charset;
}

/*
**	Level parameter to Content-Type
*/
PUBLIC HTLevel HTAnchor_level (HTParentAnchor * me)
{
    return me ? me->level : NULL;
}

PUBLIC void HTAnchor_setLevel (HTParentAnchor * me, HTLevel level)
{
    if (me) me->level = level;
}

/*
**	Content Encoding
*/
PUBLIC HTEncoding HTAnchor_encoding (HTParentAnchor * me)
{
    return me ? me->content_encoding : NULL;
}

PUBLIC void HTAnchor_setEncoding (HTParentAnchor * me, HTEncoding encoding)
{
    if (me) me->content_encoding = encoding;
}

/*
**	Content Language
**	@@@ SHOULD BE A LIST @@@
*/
PUBLIC HTLanguage HTAnchor_language (HTParentAnchor * me)
{
    return me ? me->content_language : NULL;
}

PUBLIC void HTAnchor_setLanguage (HTParentAnchor * me, HTLanguage language)
{
    if (me) me->content_language = language;
}

/*
**	Content Transfer Encoding
*/
PUBLIC HTCte HTAnchor_cte (HTParentAnchor * me)
{
    return me ? me->cte : NULL;
}

PUBLIC void HTAnchor_setCte (HTParentAnchor * me, HTCte cte)
{
    if (me) me->cte = cte;
}

/*
**	Content Length
*/
PUBLIC long int HTAnchor_length (HTParentAnchor * me)
{
    return me ? me->content_length : -1;
}

PUBLIC void HTAnchor_setLength (HTParentAnchor * me, long int length)
{
    if (me) me->content_length = length;
}

/*
**	Allowed methods	(Allow)
*/
PUBLIC int HTAnchor_methods (HTParentAnchor * me)
{
    return me ? me->methods : METHOD_INVALID;
}

PUBLIC void HTAnchor_setMethods (HTParentAnchor * me, int methodset)
{
    if (me) me->methods = methodset;
}

PUBLIC void HTAnchor_appendMethods (HTParentAnchor * me, int methodset)
{
    if (me) me->methods |= methodset;
}

/*
**	Title
*/
PUBLIC CONST char * HTAnchor_title  (HTParentAnchor * me)
{
    return me ? me->title : NULL;
}

PUBLIC void HTAnchor_setTitle (HTParentAnchor * me, CONST char * title)
{
    if (me && title) StrAllocCopy(me->title, title);
}

PUBLIC void HTAnchor_appendTitle (HTParentAnchor * me, CONST char * title)
{
    if (me && title) StrAllocCat(me->title, title);
}

/*
**	Version
*/
PUBLIC CONST char * HTAnchor_version (HTParentAnchor * me)
{
    return me ? me->version : NULL;
}

PUBLIC void HTAnchor_setVersion (HTParentAnchor * me, CONST char * version)
{
    if (me && version) StrAllocCopy(me->version, version);
}

/*
**	Derived from
*/
PUBLIC CONST char * HTAnchor_derived (HTParentAnchor * me)
{
    return me ? me->derived_from : NULL;
}

PUBLIC void HTAnchor_setDerived (HTParentAnchor * me, CONST char *derived_from)
{
    if (me && derived_from) StrAllocCopy(me->derived_from, derived_from);
}

/*
**	Date
*/
PUBLIC void HTAnchor_setDate (HTParentAnchor * me, CONST time_t * date)
{
    if (me) me->date = *date;
}

/*
**	Expires
*/
PUBLIC void HTAnchor_setExpires (HTParentAnchor * me, CONST time_t * expires)
{
    if (me) me->expires = *expires;
}

/*
**	Last Modified
*/
PUBLIC void HTAnchor_setLastModified (HTParentAnchor * me, CONST time_t * lm)
{
    if (me) me->last_modified = *lm;
}

/*
**	Extra Header List of unknown headers
*/
PUBLIC HTList * HTAnchor_Extra  (HTParentAnchor * me)
{
    return me ? me->extra_headers : NULL;
}

PUBLIC void HTAnchor_addExtra (HTParentAnchor * me, CONST char * header)
{
    if (me) {
	char *newhead = NULL;
	StrAllocCopy(newhead, header);
	if (!me->extra_headers)
	    me->extra_headers = HTList_new();
	HTList_addObject(me->extra_headers, (void *) newhead);
    }
}

/*
**	Has header been parsed?
*/
PUBLIC BOOL HTAnchor_headerParsed (HTParentAnchor * me)
{
    return (me ? me->header_parsed : NO);
}

PUBLIC void HTAnchor_setHeaderParsed (HTParentAnchor * me)
{
    if (me) me->header_parsed = YES;
}

/*	Clear Header Information
**	------------------------
*/
PUBLIC void HTAnchor_clearHeader (HTParentAnchor * me)
{
    me->methods = METHOD_INVALID;
    me->content_encoding = NULL;
#ifdef NEW_CODE
    /* WAIT UNTIL WE HANDLE LANGUAGE AS A LIST */
    if (me->content_language) {
	HTList_delete(me->content_language);
	me->content_language = HTList_new();
    }
#else
    me->content_language = NULL;
#endif
    me->content_length = -1;					  /* Invalid */
    me->cte = NULL;
    me->content_type = WWW_UNKNOWN;
    me->charset = NULL;
    me->level = NULL;
    
    me->date = (time_t) -1;
    me->expires = (time_t) -1;
    me->last_modified = (time_t) -1;
    
    FREE(me->derived_from);
    FREE(me->version);

    if (me->extra_headers) {
	HTList *cur = me->extra_headers;
	char *pres;
	while ((pres = (char *) HTList_nextObject(cur)))
	    free(pres);
	HTList_delete(me->extra_headers);
	me->extra_headers = NULL;
    }
    me->header_parsed = NO;				      /* All cleared */
}
