/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef yoml_parser_h
#define yoml_parser_h

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>
#include "yoml.h"

static yoml_t *yoml__parse_node(yaml_parser_t *parser, yaml_event_type_t *last_event, void *(*mem_set)(void *, int, size_t),
                                const char *filename);

static inline char *yoml__strdup(yaml_char_t *s)
{
    return strdup((char *)s);
}

static inline yoml_t *yoml__new_node(const char *filename, yoml_type_t type, size_t sz, yaml_char_t *anchor, yaml_event_t *event)
{
    yoml_t *node = malloc(sz);
    node->filename = filename != NULL ? strdup(filename) : NULL;
    node->type = type;
    node->line = event->start_mark.line;
    node->column = event->start_mark.column;
    node->anchor = anchor != NULL ? yoml__strdup(anchor) : NULL;
    node->_refcnt = 1;
    return node;
}

static inline yoml_t *yoml__parse_sequence(yaml_parser_t *parser, yaml_event_t *event, void *(*mem_set)(void *, int, size_t),
                                           const char *filename)
{
    yoml_t *seq = yoml__new_node(filename, YOML_TYPE_SEQUENCE, offsetof(yoml_t, data.sequence.elements),
                                 event->data.sequence_start.anchor, event);

    seq->data.sequence.size = 0;

    while (1) {
        yoml_t *new_node;
        yaml_event_type_t unhandled;
        if ((new_node = yoml__parse_node(parser, &unhandled, mem_set, filename)) == NULL) {
            if (unhandled == YAML_SEQUENCE_END_EVENT) {
                break;
            } else {
                yoml_free(seq, mem_set);
                seq = NULL;
                break;
            }
        }
        seq = realloc(seq, offsetof(yoml_t, data.sequence.elements) + sizeof(yoml_t *) * (seq->data.sequence.size + 1));
        seq->data.sequence.elements[seq->data.sequence.size++] = new_node;
    }

    return seq;
}

static inline yoml_t *yoml__parse_mapping(yaml_parser_t *parser, yaml_event_t *event, void *(*mem_set)(void *, int, size_t),
                                          const char *filename)
{
    yoml_t *map = yoml__new_node(filename, YOML_TYPE_MAPPING, offsetof(yoml_t, data.mapping.elements),
                                 event->data.mapping_start.anchor, event);

    map->data.mapping.size = 0;

    while (1) {
        yoml_t *key, *value;
        yaml_event_type_t unhandled;
        if ((key = yoml__parse_node(parser, &unhandled, mem_set, filename)) == NULL) {
            if (unhandled == YAML_MAPPING_END_EVENT) {
                break;
            } else {
                yoml_free(map, mem_set);
                map = NULL;
                break;
            }
        }
        if ((value = yoml__parse_node(parser, NULL, mem_set, filename)) == NULL) {
            yoml_free(map, mem_set);
            map = NULL;
            break;
        }
        map = realloc(map, offsetof(yoml_t, data.mapping.elements) + sizeof(yoml_mapping_element_t) * (map->data.mapping.size + 1));
        map->data.mapping.elements[map->data.mapping.size].key = key;
        map->data.mapping.elements[map->data.mapping.size].value = value;
        ++map->data.mapping.size;
    }

    return map;
}

static yoml_t *yoml__parse_node(yaml_parser_t *parser, yaml_event_type_t *unhandled, void *(*mem_set)(void *, int, size_t),
                                const char *filename)
{
    yoml_t *node;
    yaml_event_t event;

    if (unhandled != NULL)
        *unhandled = YAML_NO_EVENT;

    /* wait for a node that is not a stream/doc start event */
    while (1) {
        if (!yaml_parser_parse(parser, &event))
            return NULL;
        if (!(event.type == YAML_STREAM_START_EVENT || event.type == YAML_DOCUMENT_START_EVENT))
            break;
        yaml_event_delete(&event);
    }

    switch (event.type) {
    case YAML_ALIAS_EVENT:
        node = yoml__new_node(filename, YOML__TYPE_UNRESOLVED_ALIAS, sizeof(*node), NULL, &event);
        node->data.alias = yoml__strdup(event.data.alias.anchor);
        break;
    case YAML_SCALAR_EVENT:
        node = yoml__new_node(filename, YOML_TYPE_SCALAR, sizeof(*node), event.data.scalar.anchor, &event);
        node->data.scalar = yoml__strdup(event.data.scalar.value);
        if (mem_set != NULL)
            mem_set(event.data.scalar.value, 'A', strlen(node->data.scalar));
        break;
    case YAML_SEQUENCE_START_EVENT:
        node = yoml__parse_sequence(parser, &event, mem_set, filename);
        break;
    case YAML_MAPPING_START_EVENT:
        node = yoml__parse_mapping(parser, &event, mem_set, filename);
        break;
    default:
        node = NULL;
        if (unhandled != NULL)
            *unhandled = event.type;
        break;
    }

    yaml_event_delete(&event);

    return node;
}

static inline int yoml__merge(yoml_t **dest, size_t offset, yoml_t *src)
{
    yoml_t *key, *value;
    size_t i, j;

    if (src->type != YOML_TYPE_MAPPING)
        return -1;

    if (src->data.mapping.size != 0) {
        i = src->data.mapping.size;
        do {
            --i;
            key = src->data.mapping.elements[i].key;
            value = src->data.mapping.elements[i].value;
            if (key->type == YOML_TYPE_SCALAR) {
                for (j = 0; j != (*dest)->data.mapping.size; ++j) {
                    if ((*dest)->data.mapping.elements[j].key->type == YOML_TYPE_SCALAR &&
                        strcmp((*dest)->data.mapping.elements[j].key->data.scalar, key->data.scalar) == 0)
                        goto Skip;
                }
            }
            *dest = realloc(*dest, offsetof(yoml_t, data.mapping.elements) +
                                       ((*dest)->data.mapping.size + 1) * sizeof((*dest)->data.mapping.elements[0]));
            memmove((*dest)->data.mapping.elements + offset + 1, (*dest)->data.mapping.elements + offset,
                    ((*dest)->data.mapping.size - offset) * sizeof((*dest)->data.mapping.elements[0]));
            ++(*dest)->data.mapping.size;
            (*dest)->data.mapping.elements[offset].key = key;
            ++key->_refcnt;
            (*dest)->data.mapping.elements[offset].value = value;
            ++value->_refcnt;
        Skip:
            ;
        } while (i != 0);
    }

    return 0;
}

static inline int yoml__resolve_alias(yoml_t **target, yoml_t *doc, yaml_parser_t *parser, void *(*mem_set)(void *, int, size_t))
{
    size_t i, j;

    switch ((*target)->type) {
    case YOML_TYPE_SCALAR:
        break;
    case YOML_TYPE_SEQUENCE:
        for (i = 0; i != (*target)->data.sequence.size; ++i) {
            if (yoml__resolve_alias((*target)->data.sequence.elements + i, doc, parser, mem_set) != 0)
                return -1;
        }
        break;
    case YOML_TYPE_MAPPING:
        /* traverse in descending order (for ease of merge) */
        if ((*target)->data.mapping.size != 0) {
            i = (*target)->data.mapping.size;
            do {
                --i;
                /* merge the value */
                if (yoml__resolve_alias(&(*target)->data.mapping.elements[i].value, doc, parser, mem_set) != 0)
                    return -1;
                /* merge the keys or resolve the alias */
                if ((*target)->data.mapping.elements[i].key->type == YOML_TYPE_SCALAR &&
                    strcmp((*target)->data.mapping.elements[i].key->data.scalar, "<<") == 0) {
                    /* erase the slot (as well as preserving the values) */
                    yoml_mapping_element_t src = (*target)->data.mapping.elements[i];
                    memmove((*target)->data.mapping.elements + i, (*target)->data.mapping.elements + i + 1,
                            ((*target)->data.mapping.size - i - 1) * sizeof((*target)->data.mapping.elements[0]));
                    --(*target)->data.mapping.size;
                    /* merge */
                    if (src.value->type == YOML_TYPE_SEQUENCE) {
                        for (j = 0; j != src.value->data.sequence.size; ++j)
                            if (yoml__merge(target, i, src.value->data.sequence.elements[j]) != 0) {
                            MergeError:
                                if (parser != NULL) {
                                    parser->problem = "value of the merge key MUST be a mapping or a sequence of mappings";
                                    parser->problem_mark.line = src.key->line;
                                    parser->problem_mark.column = src.key->column;
                                }
                                return -1;
                            }
                    } else {
                        if (yoml__merge(target, i, src.value) != 0)
                            goto MergeError;
                    }
                    /* cleanup */
                    yoml_free(src.key, mem_set);
                    yoml_free(src.value, mem_set);
                } else if (yoml__resolve_alias(&(*target)->data.mapping.elements[i].key, doc, parser, mem_set) != 0) {
                    return -1;
                }
            } while (i != 0);
        }
        break;
    case YOML__TYPE_UNRESOLVED_ALIAS: {
        yoml_t *node = yoml_find_anchor(doc, (*target)->data.alias);
        if (node == NULL) {
            if (parser != NULL) {
                parser->problem = "could not to resolve the alias";
                parser->problem_mark.line = (*target)->line;
                parser->problem_mark.column = (*target)->column;
            }
            return -1;
        }
        yoml_free(*target, mem_set);
        *target = node;
        ++node->_refcnt;
    } break;
    }

    return 0;
}

static inline yoml_t *yoml_parse_document(yaml_parser_t *parser, yaml_event_type_t *unhandled,
                                          void *(*mem_set)(void *, int, size_t), const char *filename)
{
    yoml_t *doc;

    /* parse */
    if ((doc = yoml__parse_node(parser, unhandled, mem_set, filename)) == NULL) {
        return NULL;
    }
    if (unhandled != NULL)
        *unhandled = YAML_NO_EVENT;

    /* resolve aliases */
    if (yoml__resolve_alias(&doc, doc, parser, mem_set) != 0) {
        yoml_free(doc, mem_set);
        doc = NULL;
    }

    return doc;
}

#ifdef __cplusplus
}
#endif

#endif
