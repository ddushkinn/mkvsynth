#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "delbrot.h"
#include "y.tab.h"

#define UNDEFINED(n) n->type == typeVar && n->var->value == NULL

/* useful for error messages */
static char *typeNames[] = {"integer", "identifier", "string", "function", "variable", "opt argument", "operation"};

/* assign a variable */
ASTnode* assign(ASTnode *varNode, ASTnode *valueNode) {
    if (varNode->type != typeVar)
        yyerror("can't assign to a constant value (got %s)", typeNames[varNode->type]);

    /* new variable */
    if (UNDEFINED(varNode)) {
        /* allocate space for ASTnode */
        /* don't use newNode() because we don't want this node to ever be freed */
        if ((varNode->var->value = malloc(sizeof(ASTnode))) == NULL)
            yyerror("out of memory");
        
    }
    /* copy new value */
    return memcpy(varNode->var->value, valueNode, sizeof(ASTnode));
}

/* resolve an identifier */
ASTnode *identify(ASTnode *p) {
    varRec *scope = p->scope;
    varRec *v; funcRec *f;
    /* function */
    if ((f = getFn(p->str)) != NULL) {
        p->type = typeFn;
        p->fn = f;
    }
    /* existing variable */
    else if ((v = getVar(p->str, scope)) != NULL) {
        p->type = typeVar;
        p->var = v;
        
    }
    /* new variable */
    else {
        p->type = typeVar;
        p->var = putVar(p->str);
    }
    return p;
}

/* dereference a variable */
ASTnode *dereference(ASTnode *p) {
    ASTnode *next = p->next;
    memcpy(p, p->var->value, sizeof(ASTnode));
    p->next = next;
    return p;
}

ASTnode* allocateProtected(ASTnode *p) {
    if (!p)
        return NULL;

    /* allocate new node outside of the garbage collection table */
    ASTnode *dup;
    if ((dup = malloc(sizeof(ASTnode))) == NULL)
        yyerror("out of memory");

    /* recurse to children, if any */
    if (p->type == typeOp) {
        int i;
        for (i = 0; i < p->op.nops; i++)
            p->op.ops[i] = allocateProtected(p->op.ops[i]);
    }

    memcpy(dup, p, sizeof(ASTnode));
    dup->readonly = 1;
    /* recurse to linked node */
    dup->next = allocateProtected(dup->next);
    
    return dup;
}

void funcDefine(ASTnode *nameNode, ASTnode *paramNode, ASTnode *bodyNode) {
    if (nameNode->type != typeId)
        yyerror("function name \"%s\" is already in use", nameNode->var->name);
    /* create new function table entry */
    funcRec *fn;
    if ((fn = malloc(sizeof(funcRec))) == NULL)
        yyerror("out of memory");
    fn->name = nameNode->str;
    fn->body = allocateProtected(bodyNode);
    putFn(fn);
    /* allocate variables */
    for(; paramNode; paramNode = paramNode->next) {
        varRec *v = (varRec *) malloc(sizeof (varRec));
        v->name = strdup(paramNode->str);
        v->value = newNode(0);
        v->value->type = paramNode->type;
        v->next = fn->localVars;
        fn->localVars = v;
    }
}

ASTnode* userDefFnCall(ASTnode *p, ASTnode *fnNode, ASTnode *args) {
    /* determine number of arguments */
    int i, numArgs = 0;
    varRec* fnVars;
    for(fnVars = fnNode->fn->localVars; fnVars; fnVars = fnVars->next, numArgs++);
    fnVars = fnNode->fn->localVars;

    /* define and assign local variables */
    for(i = 0; i < numArgs; i++, args = args->next, fnVars = fnVars->next) {
        if (!args)
            yyerror("%s expected %d argument(s), got %d", fnNode->fn->name, numArgs, i);
        if (args->type != fnVars->value->type)
            yyerror("type mismatch in function call: arg %d expected %s, got %s", i+1, typeNames[args->type], typeNames[fnVars->value->type]);

        memcpy(fnVars->value, args, sizeof(ASTnode));
    }

    /* excessive arguments */
    if (args) {
        while ((args = args->next) != NULL) i++;
        yyerror("%s expected %d argument(s), got %d", fnNode->fn->name, numArgs, ++i);
    }

    /* execute function body */
    p = ex(fnNode->fn->body);

    return p;
}

/* handle function calls */
ASTnode* fnctCall(ASTnode *p, ASTnode *fnNode, ASTnode *args) {
    if (UNDEFINED(fnNode))
        yyerror("reference to undefined function \"%s\"", fnNode->var->name);
    if (fnNode->type != typeFn)
        yyerror("expected function name before '(' (got %s)", typeNames[fnNode->type]);

    if (fnNode->fn->ptr)
        p = (*(fnNode->fn->ptr))(p, args);
    else
        p = userDefFnCall(p, fnNode, args);

    return p;
}

/* execute a section of the AST */
ASTnode* ex(ASTnode *n) {
    if (!n)
        return NULL;

    ASTnode *p = n;
    if (n->readonly) {
        /* don't reduce node */
        p = newNode(1);
        memcpy(p, n, sizeof(ASTnode));
    }

    /* resolve identifiers */
    if (p->type == typeId)
        return identify(p);

    /* dereference variables */
    if (p->type == typeVar)
        return dereference(p);

    /* only nodes with children should be evaluated */
    if (p->type != typeOp)
        return p;

    /* for convenience/readability */
    ASTnode **child = p->op.ops;
    p->type = typeVal; /* can be redefined later if necessary */

    switch(p->op.oper) {
        /* declarations */
        case FNDEF: funcDefine(child[0], child[1], child[2]); return p;
        /* keywords */
        case IF:    if (ex(child[0])->val) ex(child[1]); else if (p->op.nops > 2) ex(child[2]); p->type = typeOp; return p;
        case WHILE: while(ex(child[0])->val) { ex(child[1]); freeNodes(1); } freeNodes(1); p->type = typeOp; return p;
        case FOR:   for(ex(child[0]); ex(child[1])->val; ex(child[2]), freeNodes(1)) ex(child[3]); freeNodes(1); p->type = typeOp; return p;
        /* functions */
        case FNCT:  return fnctCall(p, ex(child[0]), child[1]);
        case '.':   child[0]->next = child[2]; return fnctCall(p, ex(child[1]), ex(child[0]));
        /* assignment */
        case '=':   return assign(ex(child[0]), ex(child[1])); 
        case ADDEQ: return modvar(ex(child[0]), '+', ex(child[1])->val);
        case SUBEQ: return modvar(ex(child[0]), '-', ex(child[1])->val);
        case MULEQ: return modvar(ex(child[0]), '*', ex(child[1])->val);
        case DIVEQ: return modvar(ex(child[0]), '/', ex(child[1])->val);
        case MODEQ: return modvar(ex(child[0]), '%', ex(child[1])->val);
        case INC:   return modvar(ex(child[0]), '+', 1);
        case DEC:   return modvar(ex(child[0]), '-', 1);
        /* arithmetic operators */
        /* TODO: make these real function calls, complete with type checking */
        case '%':   p->val = (int) ex(child[0])->val % (int) ex(child[1])->val; return p;
        case '^':   p->val = pow(ex(child[0])->val, ex(child[1])->val); return p;
        case '*':   p->val = ex(child[0])->val * ex(child[1])->val;  return p;
        case '/':   p->val = ex(child[0])->val / ex(child[1])->val;  return p;
        case '+':   p->val = ex(child[0])->val + ex(child[1])->val;  return p;
        case '-':   p->val = ex(child[0])->val - ex(child[1])->val;  return p;
        case NEG:   p->val = -ex(child[0])->val;                     return p;
        /* boolean operators */
        case '!':   p->val = !ex(child[0])->val;                     return p;
        case '>':   p->val = ex(child[0])->val > ex(child[1])->val;  return p;
        case '<':   p->val = ex(child[0])->val < ex(child[1])->val;  return p;
        case GE:    p->val = ex(child[0])->val >= ex(child[1])->val; return p;
        case LE:    p->val = ex(child[0])->val <= ex(child[1])->val; return p;
        case EQ:    p->val = ex(child[0])->val == ex(child[1])->val; return p;
        case NE:    p->val = ex(child[0])->val != ex(child[1])->val; return p;
        case LOR:   p->val = ex(child[0])->val || ex(child[1])->val; return p;
        case LAND:  p->val = ex(child[0])->val && ex(child[1])->val; return p;
        /* compound statements */
        case ';':   ex(child[0]); return ex(child[1]);
    }
    /* should never wind up here */
    yyerror("Unknown operator");
}

/* helper function to ensure that a function call is valid */
/* TODO: use ... to allow type checking */
void checkArgs(char *funcName, ASTnode *args, int numArgs, ...) {
    int i;
    va_list ap;
    va_start(ap, numArgs);
    ASTnode *traverse = args;
    /* check for missing/uninitialized/mistyped arguments */
    for (i = 0; i < numArgs; i++) {
        if (traverse == NULL)
            yyerror("%s expected %d argument(s), got %d", funcName, numArgs, i);
        if (UNDEFINED(traverse))
            yyerror("reference to undefined variable \"%s\"", traverse->var->name);
        int argType = va_arg(ap, int);
        if (ex(traverse)->type != argType)
            yyerror("type mismatch in function call: arg %d expected %s, got %s", i+1, typeNames[argType], typeNames[traverse->type]);
        traverse = traverse->next;
    }
    va_end(ap);
    /* check for excess arguments */
    if (traverse != NULL && traverse->type != typeOptArg) {
        while ((traverse = traverse->next) != NULL) i++;
        yyerror("%s expected %d argument(s), got %d", funcName, numArgs, ++i);
    }
}

/* standard mathematical functions, modified to use ASTnode */
ASTnode* nsin (ASTnode *p, ASTnode *args) { checkArgs("sin", args, 1, typeVal); p->val = sin(ex(args)->val);  return p; }
ASTnode* ncos (ASTnode *p, ASTnode *args) { checkArgs("cos", args, 1, typeVal); p->val = cos(ex(args)->val);  return p; }
ASTnode* nlog (ASTnode *p, ASTnode *args) { checkArgs("log", args, 1, typeVal); p->val = log(ex(args)->val);  return p; }
ASTnode* nsqrt(ASTnode *p, ASTnode *args) { checkArgs("sqrt",args, 1, typeVal); p->val = sqrt(ex(args)->val); return p; }

/* modify the value of a variable */
ASTnode* modvar(ASTnode *varNode, char op, double mod) {
    if(UNDEFINED(varNode))
        yyerror("reference to uninitialized variable \"%s\"", varNode->var->name);
    if(varNode->type != typeVar)
        yyerror("can't modify constant");
    if(varNode->var->value->type != typeVal)
        yyerror("can't modify non-numeric variable \"%s\"", varNode->var->name);

    switch (op) {
        case '+': varNode->var->value->val += mod; break;
        case '-': varNode->var->value->val -= mod; break;
        case '*': varNode->var->value->val *= mod; break;
        case '/': varNode->var->value->val /= mod; break;
        case '%': varNode->var->value->val = ((int)varNode->var->value->val % (int)mod); break;
    }
    return ex(varNode);
}

/* helper function to get optional arguments in a function call */
/* TODO: add type checking */
void* getOptArg(ASTnode *args, char *name, int type) {
    ASTnode *traverse = args;
    for (traverse = args; traverse != NULL; traverse = traverse->next)
        if (traverse->type == typeOptArg && !(strncmp(traverse->var->name,name,strlen(name)))) {
            ASTnode *value = ex(traverse->var->value);
            switch (type) {
                case typeVal: return &value->val;
                case typeStr: return value->str;
            }
        }
    return NULL;
}

/* helper function to interpret string literals */
char* unesc(char* str) {
    int i, j;
    for (i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\\') {
            switch (str[i+1]) {
                case 't': str[i] = '\t'; break;
                case 'n': str[i] = '\n'; break;
                case 'r': str[i] = '\r'; break;
                case '\\':str[i] = '\\'; break;
                case '\'':str[i] = '\''; break;
                case '\"':str[i] = '\"'; break;
                default: yyerror("unknown literal \"\\%c\"", str[i+1]);
            }
            for (j = i + 1; str[j] != '\0'; j++)
                str[j] = str[j+1];
        }
    }
    return str;
}

/* generalized print function; will print any number of args */
ASTnode* print(ASTnode *p, ASTnode *args) {
    while(args) { 
        /* unreduced/unprintable types */
        if (UNDEFINED(args))
            yyerror("reference to uninitialized variable \"%s\"", args->var->name);
        if (args->type == typeId)
            args = ex(args);
        if (args->type == typeVar || args->type == typeOp)
            args = ex(args);
        /* printable types */
        switch(args->type) {
            case typeVal: printf("%.10g ", args->val); break;
            case typeStr: printf("%s ", unesc(args->str)); break;
            default: /* should never wind up here */ break;
        }
        args = args->next;
    }
    printf("\n");
    return p;
}

void ffmpegDecode(char *filename, int numFrames) {
    if (numFrames != -1)
        printf("decoded %d frames of %s\n", numFrames, filename);
    else
        printf("decoded %s\n", filename);
}

/* toy ffmpeg decoding function, showcasing optional arguments */
ASTnode* ffmpegDecode_AST(ASTnode *p, ASTnode *args) {
    checkArgs("ffmpegDecode", args, 1, typeStr);

    char *str = args->str;
    double frames = OPTVAL("frames", -1);

    ffmpegDecode(str, frames);

    RETURNVAL(0);
}