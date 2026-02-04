#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_xmlschema.h"
#include "catalog/pg_namespace.h"
#include "commands/xmlschemacmds.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/xml.h"


/*
 * CREATE XMLSCHEMA
 */
ObjectAddress
DefineXmlSchema(ParseState *pstate, List *names, List *parameters, bool if_not_exists)
{
	char	   *schemaName;
	Oid			schemaNamespace;
	AclResult	aclresult;
	ListCell   *pl;
	DefElem    *schemaDataEl = NULL;
	xmltype    *schemaData;
	Oid			newoid;
	ObjectAddress address;

	schemaNamespace = QualifiedNameGetCreationNamespace(names, &schemaName);

	aclresult = object_aclcheck(NamespaceRelationId, schemaNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(schemaNamespace));

	/* Parse parameters */
	foreach(pl, parameters)
	{
		DefElem    *defel = lfirst_node(DefElem, pl);
		DefElem   **defelp;

		if (strcmp(defel->defname, "schema") == 0)
			defelp = &schemaDataEl;
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("XML schema attribute \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
			break;
		}
		if (*defelp != NULL)
			errorConflictingDefElem(defel, pstate);
		*defelp = defel;
	}

	if (!schemaDataEl)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("parameter \"schema\" must be specified")));

	/* The schema definition is passed as a String node from the grammar */
	schemaData = (xmltype *) cstring_to_text(defGetString(schemaDataEl));

	newoid = XmlSchemaCreate(schemaName,
							 schemaNamespace,
							 GetUserId(),
							 schemaData,
							 if_not_exists,
							 false);

	if (!OidIsValid(newoid))
	{
		/*
		 * When IF NOT EXISTS was specified and the object already existed,
		 * XmlSchemaCreate returned InvalidOid. Report an invalid object
		 * address.
		 */
		address.classId = XmlSchemaRelationId;
		address.objectId = InvalidOid;
		address.objectSubId = 0;
		return address;
	}

	ObjectAddressSet(address, XmlSchemaRelationId, newoid);

	return address;
}
