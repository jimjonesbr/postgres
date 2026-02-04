#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_xmlschema.h"
#include "catalog/pg_namespace.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/xml.h"

#ifdef USE_LIBXML
#include <libxml/xmlschemas.h>
#endif

/*
 * XmlSchemaCreate
 *
 * Add a new tuple to pg_xmlschema.
 *
 * if_not_exists: if true, don't fail on duplicate name, just print a notice
 * and return InvalidOid.
 * quiet: if true, don't fail on duplicate name, just silently return
 * InvalidOid (which overides if_not_exists).
 */
Oid
XmlSchemaCreate(const char *schemaname,
				Oid schemanamespace,
				Oid schemaowner,
				xmltype *schemadata,
				bool if_not_exists,
				bool quiet)
{
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	Datum		values[Natts_pg_xmlschema];
	bool		nulls[Natts_pg_xmlschema];
	NameData	name_name;
	Oid			oid;
	ObjectAddress myself,
				referenced;

	Assert(schemaname);
	Assert(schemanamespace);
	Assert(schemaowner);
	Assert(schemadata);

#ifdef USE_LIBXML
	/* Validate the XML Schema before storing it */
	{
		xmlSchemaParserCtxtPtr parser_ctxt;
		xmlSchemaPtr schema_ptr;
		PgXmlErrorContext *xmlerrcxt;
		char	   *schemastr;

		schemastr = text_to_cstring(schemadata);
		xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_WELLFORMED);

		PG_TRY();
		{
			parser_ctxt = xmlSchemaNewMemParserCtxt(schemastr, strlen(schemastr));
			if (parser_ctxt == NULL)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
							"failed to create schema parser context");

			schema_ptr = xmlSchemaParse(parser_ctxt);
			if (schema_ptr == NULL)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
							"invalid XML schema definition");

			xmlSchemaFree(schema_ptr);
			xmlSchemaFreeParserCtxt(parser_ctxt);
		}
		PG_CATCH();
		{
			pg_xml_done(xmlerrcxt, true);
			PG_RE_THROW();
		}
		PG_END_TRY();

		pg_xml_done(xmlerrcxt, false);
		pfree(schemastr);
	}
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("xmlschema support requires libxml")));
#endif

	/*
	 * Make sure there is no existing XML schema of same name in the
	 * namespace.
	 *
	 * This would be caught by the unique index anyway; we're just giving a
	 * friendlier error message.  The unique index provides a backstop against
	 * race conditions.
	 */
	oid = GetSysCacheOid2(XMLSCHEMANAMENSP,
						  Anum_pg_xmlschema_oid,
						  PointerGetDatum(schemaname),
						  ObjectIdGetDatum(schemanamespace));
	if (OidIsValid(oid))
	{
		if (quiet)
			return InvalidOid;
		else if (if_not_exists)
		{
			/*
			 * If we are in an extension script, insist that the pre-existing
			 * object be a member of the extension, to avoid security risks.
			 */
			ObjectAddressSet(myself, XmlSchemaRelationId, oid);
			checkMembershipInCurrentExtension(&myself);

			/* OK to skip */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("XML schema \"%s\" already exists, skipping",
							schemaname)));
			return InvalidOid;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("XML schema \"%s\" already exists",
							schemaname)));
	}

	/* open pg_xmlschema; lock to protect against concurrent changes */
	rel = table_open(XmlSchemaRelationId, ShareRowExclusiveLock);

	tupDesc = RelationGetDescr(rel);

	/* form a tuple */
	memset(nulls, 0, sizeof(nulls));

	namestrcpy(&name_name, schemaname);
	oid = GetNewOidWithIndex(rel, XmlSchemaOidIndexId,
							 Anum_pg_xmlschema_oid);
	values[Anum_pg_xmlschema_oid - 1] = ObjectIdGetDatum(oid);
	values[Anum_pg_xmlschema_schemaname - 1] = NameGetDatum(&name_name);
	values[Anum_pg_xmlschema_schemanamespace - 1] = ObjectIdGetDatum(schemanamespace);
	values[Anum_pg_xmlschema_schemaowner - 1] = ObjectIdGetDatum(schemaowner);
	values[Anum_pg_xmlschema_schemadata - 1] = PointerGetDatum(schemadata);

	/* Set up default ACL */
	{
		Acl		   *schemaacl;

		schemaacl = get_user_default_acl(OBJECT_XMLSCHEMA, schemaowner,
										 schemanamespace);
		if (schemaacl != NULL)
			values[Anum_pg_xmlschema_schemaacl - 1] = PointerGetDatum(schemaacl);
		else
			nulls[Anum_pg_xmlschema_schemaacl - 1] = true;
	}

	tup = heap_form_tuple(tupDesc, values, nulls);

	/* insert a new tuple */
	CatalogTupleInsert(rel, tup);
	Assert(OidIsValid(oid));

	/* set up dependencies for the new XML schema */
	myself.classId = XmlSchemaRelationId;
	myself.objectId = oid;
	myself.objectSubId = 0;

	/* create dependency on namespace */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = schemanamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* create dependency on owner */
	recordDependencyOnOwner(XmlSchemaRelationId, oid, schemaowner);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new XML schema */
	InvokeObjectPostCreateHook(XmlSchemaRelationId, oid, 0);

	heap_freetuple(tup);
	table_close(rel, NoLock);

	return oid;
}
