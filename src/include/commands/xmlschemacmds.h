#ifndef XMLSCHEMACMDS_H
#define XMLSCHEMACMDS_H

#include "catalog/objectaddress.h"
#include "parser/parse_node.h"

extern ObjectAddress DefineXmlSchema(ParseState *pstate, List *names, List *parameters, bool if_not_exists);

#endif							/* XMLSCHEMACMDS_H */
