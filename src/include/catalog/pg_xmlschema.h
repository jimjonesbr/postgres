#ifndef PG_XMLSCHEMA_H
#define PG_XMLSCHEMA_H

#include "catalog/genbki.h"
#include "catalog/pg_xmlschema_d.h"
#include "utils/xml.h"

CATALOG(pg_xmlschema,6434,XmlSchemaRelationId)
{
	Oid			oid;			/* oid */
	NameData	schemaname;		/* XML schema name */

	/* OID of namespace containing this XML schema */
	Oid			schemanamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/* owner of XML schema */
	Oid			schemaowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);
#ifdef CATALOG_VARLEN
	/* The XSD itself */
	xml			schemadata BKI_FORCE_NOT_NULL;

	/* Access privileges */
	aclitem		schemaacl[1] BKI_DEFAULT(_null_);
#endif
} FormData_pg_xmlschema;

/* ----------------
 *        Form_pg_xmlschema maps to a pointer to a row with
 *        the format of pg_xmlschema relation.
 * ----------------
 */
typedef FormData_pg_xmlschema * Form_pg_xmlschema;

DECLARE_TOAST(pg_xmlschema, 6435, 6436);
DECLARE_UNIQUE_INDEX(pg_xmlschema_name_nsp_index, 6437, XmlSchemaNameNspIndexId, pg_xmlschema, btree(schemaname name_ops, schemanamespace oid_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_xmlschema_oid_index, 6438, XmlSchemaOidIndexId, pg_xmlschema, btree(oid oid_ops));

MAKE_SYSCACHE(XMLSCHEMANAMENSP, pg_xmlschema_name_nsp_index, 8);
MAKE_SYSCACHE(XMLSCHEMAOID, pg_xmlschema_oid_index, 8);

extern Oid	XmlSchemaCreate(const char *schemaname,
							Oid schemanamespace,
							Oid schemaowner,
							xmltype *schemadata,
							bool if_not_exists,
							bool quiet);

#endif							/* PG_XMLSCHEMA_H */
