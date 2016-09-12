/******************************************************************************
 * Copyright (c) 2014-2016 Leandro T. C. Melo (ltcmelo@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 *****************************************************************************/

/*--------------------------*/
/*--- The UaiSo! Project ---*/
/*--------------------------*/

#include "Haskell/HsParser.h"
#include "Haskell/HsLexer.h"
#include "Ast/Ast.h"
#include "Common/Assert.h"
#include "Common/Trace__.h"
#include "Parsing/Lexeme.h"
#include "Parsing/ParsingContext.h"

#define TRACE_NAME "HsParser"

using namespace uaiso;

namespace uaiso
{
extern std::unordered_map<std::uint16_t, const char*> tokenName;
}

namespace
{
// Contextual keywords.
const char* const kAs = "as";
const char* const kHiding = "hiding";
const char* const kQualified = "qualified";
}

HsParser::HsParser()
{}

void HsParser::matchOrSkipTo(Token tk, const char *rule)
{
    if (!match(tk)) {
        DEBUG_TRACE("%s, skip to %s\n", rule, tokenName[tk]);
        skipTo(tk);
    }
}

bool HsParser::parse(Lexer* lexer, ParsingContext* context)
{
    UAISO_ASSERT(lexer, return false);
    UAISO_ASSERT(context && context->fileName(), return false);

    prepare(lexer, context);
    consumeToken();
    if (ahead_ == TK_EOP)
        return false;

    auto prog = std::unique_ptr<ProgramAst>(newAst<ProgramAst>());
    if (ahead_ == TK_MODULE)
        prog->setModule(parseModule());
    if (ahead_ == TK_LBRACE)
        prog->setDecls(parseBody());
    context->takeAst(std::unique_ptr<Ast>(prog.release()));

    return true;
}

Parser::Decl HsParser::parseModule()
{
    UAISO_ASSERT(ahead_ == TK_MODULE, return Decl());
    consumeToken();
    auto module = ModuleDeclAst::create();
    module->setKeyLoc(prevLoc_);
    module->setName(parseModid());
    if (ahead_ == TK_LPAREN)
        module->setExpot(parseExport());
    match(TK_WHERE);
    module->setTerminLoc(prevLoc_);

    return std::move(module);
}

Parser::Decl HsParser::parseExport()
{
    UAISO_ASSERT(ahead_ == TK_LPAREN, return Decl());
    consumeToken();
    auto expot = ExportDeclAst::create();
    expot->setLDelimLoc(prevLoc_);
    expot->setSelections(parseSelection(true));
    matchOrSkipTo(TK_RPAREN, "parseExport");
    expot->setRDelimLoc(prevLoc_);

    return std::move(expot);
}

Parser::Decl HsParser::parseImport()
{
    UAISO_ASSERT(ahead_ == TK_IMPORT, return Decl());
    consumeToken();
    auto group = ImportGroupDeclAst::create();
    group->setKeyLoc(prevLoc_);

    // Utility lambda to compare contextual keywords.
    auto matchIdent = [this] (const std::string& s) {
        if (ahead_ == TK_IDENT) {
            const Ident* ident = context_->fetchLexeme<Ident>(currentLoc().lineCol());
            return ident && ident->str() == s;
        }
        return false;
    };

    auto import = ImportDeclAst::create();
    if (matchIdent(kQualified))
        import->setMode(parseName(TK_IDENT));
    auto target = IdentExprAst::create();
    target->setName(parseModid());
    import->setTarget(std::move(target));
    if (matchIdent(kAs)) {
        consumeToken();
        import->setAsLoc(prevLoc_);
        import->setLocalName(parseModid());
    }
    if (matchIdent(kHiding))
        consumeToken(); // TODO: Store hidden names.
    if (maybeConsume(TK_LPAREN)) {
        import->setLDelimLoc(prevLoc_);
        import->setSelections(parseSelection(false));
        matchOrSkipTo(TK_RPAREN, "parseImport");
        import->setRDelimLoc(prevLoc_);
    }
    group->addModule(std::move(import));

    return std::move(group);
}

Parser::DeclList HsParser::parseSelection(bool allowModid)
{
    DeclList selects;
    do {
        auto select = ExportSelectionDeclAst::create();
        switch (ahead_) {
        case TK_RPAREN:
            break; // We're done.

        case TK_MODULE:
            consumeToken();
            if (allowModid) {
                select->setName(parseModid());
            } else {
                context_->trackReport(Diagnostic::UnexpectedToken, prevLoc_);
                if (ahead_ == TK_PROPER_IDENT)
                    parseModid(); // Parse and discard it.
            }
            break;

        case TK_LPAREN:
            select->setName(parseQVarSym());
            break;

        case TK_IDENT:
            select->setName(parseVarId());
            break;

        default:
            auto qname = NestedNameAst::create();
            do {
                addToList(qname->names_, parseConId());
            } while (maybeConsume(TK_JOKER) && ahead_ == TK_PROPER_IDENT);

            if (ahead_ == TK_IDENT) {
                addToList(qname->names_, parseVarId());
            } else if (ahead_ == TK_LPAREN) {
                consumeToken();
                if (maybeConsume(TK_DOT_DOT)) {
                    // TODO: Mark export all.
                } else {
                    do {
                        parseVarOrCon(); // TODO: Store "sub-selections".
                    } while (maybeConsume(TK_COMMA));
                }
                matchOrSkipTo(TK_RPAREN, "parseExportItem");
            }
            select->setName(std::move(qname));
            break;
        }
        addToList(selects, std::move(select));
    } while (maybeConsume(TK_COMMA));

    return selects;
}

Parser::DeclList HsParser::parseBody()
{
    UAISO_ASSERT(ahead_ == TK_LBRACE, return DeclList());
    consumeToken();
    bool wantBrace = false;
    while (ahead_ == TK_IMPORT) {
        parseImport();
        if (!maybeConsume(TK_SEMICOLON)) {
            wantBrace = true;
            break;
        }
    }
    if (!wantBrace)
        parseTopDecls();
    matchOrSkipTo(TK_RBRACE, "parseBody");

    return DeclList();
}

Parser::DeclList HsParser::parseTopDecls()
{
    do {
        switch (ahead_) {
        case TK_TYPE:
            break;

        case TK_DATA:
            break;

        case TK_NEWTYPE:
            break;

        case TK_CLASS:
            break;

        case TK_INSTANCE:
            break;

        case TK_DEFAULT:
            break;

        case TK_FOREIGN:
            break;

        default:
            parseDecl();
            break;
        }
    } while (maybeConsume(TK_SEMICOLON));

    return DeclList();
}

Parser::Decl HsParser::parseDecl()
{
    switch (ahead_) {
    case TK_SEMICOLON:
        consumeToken();
        return EmptyDeclAst::create();

    case TK_INFIX:
    case TK_INFIXL:
    case TK_INFIXR:
        // TODO: Fixity declaration.
        return Decl();

    default:
        return parsePatBindOrFuncOrTypeSig();
    }
}

Parser::Decl HsParser::parsePatBindOrFuncOrTypeSig()
{
    Name name;
    if (maybeConsume(TK_IDENT)) {
        name = SimpleNameAst::create(prevLoc_);
    } else if (ahead_ == TK_LPAREN
               && isVarSym(peekToken(2))
               && peekToken(3) == TK_RPAREN) {
        consumeToken();
        name = PuncNameAst::create(prevLoc_);
    }

    if (!name)
        return parsePatBindOrFunc();

    bool wantTySig = false;
    NameList vars;
    while (maybeConsume(TK_COMMA)) {
        addToList(vars, parseSymOrId(&HsParser::parseVarSym, &HsParser::parseVarId));
        wantTySig = true;
    }
    if (wantTySig || ahead_ == TK_COLON_COLON) {
        auto group = NameAstList::create(std::move(name));
        if (vars)
            group->merge(std::move(vars));
        return parseTypeSig(/* group */);
    }

    std::unique_ptr<PatExprAst> pat;
    if (ahead_ == TK_AT) {
        parseAsPat();
    }

    if (auto name = maybeParseQConOp()) {
        parsePat();
    }

    switch (ahead_) {
    case TK_EQ:
        // TODO: Expression.
        break;

    case TK_PIPE:
        // TODO: Guard.
        break;

    default:
        if (pat)
            return parseInfixFunc(/* pat */);
        return parseFunc(/* name */);
    }

    return Decl();
}

Parser::Decl HsParser::parsePatBindOrFunc()
{
    return Decl();
}

Parser::Decl HsParser::parseTypeSig()
{
    UAISO_ASSERT(ahead_ == TK_COLON_COLON, return Decl());
    consumeToken();
    return Decl();
}

Parser::Decl HsParser::parsePatBind()
{
    return Decl();
}

Parser::Decl HsParser::parseInfixFunc()
{
    return Decl();
}

Parser::Decl HsParser::parseFunc()
{
    return Decl();
}

    //--- Expressions ---//

Parser::Expr HsParser::parsePat()
{
    parseLPat();

    if (auto name = maybeParseQConOp()) {
        parsePat();
    }

    return Expr();
}

Parser::Expr HsParser::parseLPat()
{
    switch (ahead_) {
    case TK_MINUS:
        consumeToken();
        if (maybeConsume(TK_INT_LIT))
            return NumLitExprAst::create(prevLoc_, NumLitVariety::IntFormat);
        match(TK_FLOAT_LIT);
        return NumLitExprAst::create(prevLoc_, NumLitVariety::FloatFormat);

    case TK_LBRACKET:
        return parseListConOrLitPat();

    case TK_LPAREN: {
        const Token peek = peekToken(2);
        if (peek == TK_PROPER_IDENT || isConSym(peek)) {
            parseQCon();
            parsePatList();
            return CallExprAst::create();
        }
        return parseTupleConOrLitOrWrapOrUnitPat();
    }

    case TK_PROPER_IDENT:
        parseQConId();
        parsePatList();
        return CallExprAst::create();

    default:
        return parseAPat();
    }
}

Parser::Expr HsParser::parseAPat()
{
    switch (ahead_) {
    case TK_INT_LIT:
        return parseIntLit();

    case TK_FLOAT_LIT:
        return parseFloatLit();

    case TK_TRUE_VALUE:
    case TK_FALSE_VALUE:
        return parseBoolLit();

    case TK_CHAR_LIT:
        return parseCharLit();

    case TK_STR_LIT:
        return parseStrLit();

    case TK_UNDERSCORE:
        consumeToken();
        return WildCardExprAst::create(prevLoc_);

    case TK_TILDE:
        // TODO: Irrefutable pattern.
        return Expr();

    default:
        break;
    }

    return Expr();
}

Parser::Expr HsParser::parseListConOrLitPat()
{
    UAISO_ASSERT(ahead_ == TK_LBRACKET, return Expr());
    consumeToken();

    if (maybeConsume(TK_RBRACKET))
        return CallExprAst::create(); // List's data con `[ ]'.

    parsePatDList();
    matchOrSkipTo(TK_RBRACKET, "parseListConOrLit");
    return Expr();
}

Parser::Expr HsParser::parseTupleConOrLitOrWrapOrUnitPat()
{
    UAISO_ASSERT(ahead_ == TK_LPAREN, return Expr());
    consumeToken();

    if (maybeConsume(TK_RPAREN))
        return NullLitExprAst::create(); // Unit value.

    size_t tupleCnt = 0;
    while (maybeConsume(TK_COMMA)) {
        ++tupleCnt;
    }
    if (tupleCnt) {
        matchOrSkipTo(TK_LPAREN, "parseTupleConOrLitOrWrapOrUnit");
        parsePatList();
        return Expr();
    }

    // Tuple literal.
    parsePatDList();
    matchOrSkipTo(TK_LPAREN, "parseTupleConOrLitOrWrapOrUnit");
    return Expr();
}

Parser::ExprList HsParser::parsePatList()
{
    return ExprList();
}

Parser::ExprList HsParser::parsePatDList()
{
    return ExprList();
}

Parser::ExprList HsParser::parseAPatList()
{
    return ExprList();
}

Parser::ExprList HsParser::parseAPatDList()
{
    return ExprList();
}

Parser::Expr HsParser::parseAsPat()
{
    UAISO_ASSERT(ahead_ == TK_AT, return Expr());
    consumeToken();
    parseAPat();
    return Expr();
}

Parser::Expr HsParser::parseExpr()
{
    return parseAExpr();
}

Parser::Expr HsParser::parseAExpr()
{
    switch (ahead_) {
    case TK_INT_LIT:
        return parseIntLit();

    case TK_FLOAT_LIT:
        return parseFloatLit();

    case TK_TRUE_VALUE:
    case TK_FALSE_VALUE:
        return parseBoolLit();

    default:
        fail();
        return Expr();
    }
}

Parser::Expr HsParser::parseIntLit()
{
    UAISO_ASSERT(ahead_ == TK_INT_LIT, return Expr());
    consumeToken();
    return NumLitExprAst::create(prevLoc_, NumLitVariety::IntFormat);
}

Parser::Expr HsParser::parseFloatLit()
{
    UAISO_ASSERT(ahead_ == TK_FLOAT_LIT, return Expr());
    consumeToken();
    return NumLitExprAst::create(prevLoc_, NumLitVariety::FloatFormat);
}

Parser::Expr HsParser::parseStrLit()
{
    UAISO_ASSERT(ahead_ == TK_STR_LIT, return Expr());
    consumeToken();
    return StrLitExprAst::create(prevLoc_);
}

Parser::Expr HsParser::parseCharLit()
{
    UAISO_ASSERT(ahead_ == TK_CHAR_LIT, return Expr());
    consumeToken();
    return CharLitExprAst::create(prevLoc_);
}

Parser::Expr HsParser::parseBoolLit()
{
    UAISO_ASSERT(ahead_ == TK_TRUE_VALUE
                 || ahead_ == TK_FALSE_VALUE, return Expr());
    consumeToken();
    return BoolLitExprAst::create(prevLoc_);
}

    //--- Names ---//

Parser::Name HsParser::parseModid()
{
    auto modid = NestedNameAst::create();
    do {
        modid->addName(parseName(TK_PROPER_IDENT));
    } while (maybeConsume(TK_JOKER));

    return std::move(modid);
}

Parser::Name HsParser::parseVarOrCon()
{
    if (maybeConsume(TK_LPAREN)) {
        Name name;
        switch (ahead_) {
        case TK_COLON:
        case TK_SPECIAL_IDENT:
            name = parseConSym();
            break;

        default:
            name = parseVarSym();
            break;
        }
        matchOrSkipTo(TK_RPAREN, "parseVarOrConName");
        return name;
    }

    if (ahead_ == TK_IDENT)
        return parseVarId();
    return parseConId();
}

Parser::Name HsParser::parseSymOrId(Name (HsParser::*parseSym)(),
                                    Name (HsParser::*parseId)())
{
    if (ahead_ == TK_LPAREN)
        return (this->*(parseSym))();
    return (this->*(parseId))();
}

Parser::Name HsParser::parseQCon()
{
    return parseSymOrId(&HsParser::parseQConSym, &HsParser::parseQConId);
}

Parser::Name HsParser::parseQVar()
{
    return parseSymOrId(&HsParser::parseQVarSym, &HsParser::parseQVarId);
}

Parser::Name HsParser::parseCon()
{
    return parseSymOrId(&HsParser::parseConSym, &HsParser::parseConId);
}

Parser::Name HsParser::parseVar()
{
    return parseSymOrId(&HsParser::parseVarSym, &HsParser::parseVarId);
}

Parser::Name HsParser::parseQConId()
{
    return parseQName(&HsParser::parseConId);
}

Parser::Name HsParser::parseQVarId()
{
    return parseQName(&HsParser::parseVarId);
}

Parser::Name HsParser::parseQConSym()
{
    UAISO_ASSERT(ahead_ == TK_LPAREN, return Name());
    consumeToken();
    auto name = parseQName(&HsParser::parseConSym);
    matchOrSkipTo(TK_RPAREN, "parseQConSymName");

    return std::move(name);
}

Parser::Name HsParser::parseQVarSym()
{
    UAISO_ASSERT(ahead_ == TK_LPAREN, return Name());
    consumeToken();
    auto name = parseQName(&HsParser::parseVarSym);
    matchOrSkipTo(TK_RPAREN, "parseQVarSymName");

    return std::move(name);
}

Parser::Name HsParser::parseConSym()
{
    if (isConSym(ahead_)) {
        consumeToken();
        return SpecialNameAst::create(prevLoc_);
    }

    fail();
    return ErrorNameAst::create(prevLoc_);
}

Parser::Name HsParser::parseVarSym()
{
    if (isVarSym(ahead_)) {
        consumeToken();
        return PuncNameAst::create(prevLoc_);
    }

    fail();
    return ErrorNameAst::create(prevLoc_);
}

Parser::Name HsParser::parseConId()
{
    return parseName(TK_PROPER_IDENT);
}

Parser::Name HsParser::parseVarId()
{
    return parseName(TK_IDENT);
}

Parser::Name HsParser::maybeParseQConOp()
{
    if (maybeConsume(TK_COLON))
        return SpecialNameAst::create(prevLoc_);

    if (ahead_ == TK_BACKTICK
            && peekToken(2) == TK_PROPER_IDENT) {
        consumeToken();
        auto name = parseQConId();
        matchOrSkipTo(TK_BACKTICK, "parseQConOp");
        return name;
    }

    if (ahead_ == TK_PROPER_IDENT)
        return parseQConSym();

    return Name();
}

Parser::Name HsParser::parseQName(Name (HsParser::*parseFunc)())
{
    auto qname = NestedNameAst::create();
    while (maybeConsume(TK_PROPER_IDENT)) {
        addToList(qname->names_, SimpleNameAst::create(prevLoc_));
        match(TK_JOKER);
    }
    addToList(qname->names_, ((this->*(parseFunc))()));

    return std::move(qname);
}

HsParser::Name HsParser::parseName(Token tk)
{
    /*
     * We cannot fail a NameAst match. This would mean an identifier without a
     * corresponding location in the lexeme map, which is not allowed in our
     * design. Therefore, an ErrorNameAst must be constructed in such case.
     */
    if (match(tk))
        return SimpleNameAst::create(prevLoc_);
    return ErrorNameAst::create(prevLoc_);
}
