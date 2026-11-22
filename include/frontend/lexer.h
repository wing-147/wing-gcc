#ifndef LEXER_H
#define LEXER_H

typedef struct SourceFile SourceFile;
typedef struct Token Token;

// Lex one borrowed source into a fully located and decoded token list. The
// source and its buffer must outlive the tokens; the function sets it as the
// current source so lexical diagnostics resolve consistently. contents[size]
// must be a readable terminating NUL; embedded NUL bytes remain permitted.
Token *tokenize(SourceFile *source);

// Load, lex, attach file/line/column metadata and decode literal values for
// `path`. Returns the token list, or NULL on I/O failure with errno preserved.
// On success the loaded file becomes current_source(); the caller releases the
// tokens with token_free() and the file with source_free(current_source()).
Token *tokenize_file(const char *path);

// Release a token list produced by tokenize()/tokenize_file().
void token_free(Token *tokens);

#endif // LEXER_H
