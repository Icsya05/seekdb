# JSON_LENGTH child batching

The real SQL engine on production AI commit 140f produced correct lengths but
made eight singleton embedding requests for eight rows when AI_EMBED was wrapped
in JSON_LENGTH. A second NULL/filter/LIMIT/path case made two singleton requests.
The corresponding direct projection already issued one multi-input request.

Root cause: JSON_LENGTH supplied only a scalar evaluator. The default batch
adapter invokes scalar evaluation per row, and the scalar JSON helper evaluates
its child with eval(), bypassing the child's native batch callback.

Two new real SQL regression tests failed against the control binary 8eeee63ee
(its AI/JSON code is identical to 140f): expected one request, observed eight and
two respectively; neither failed because of returned values or SQL errors.
An ordinary LONGTEXT JSON/path/NULL/scalar regression passed on the old binary.

The new JSON_LENGTH batch callback evaluates both unconditional arguments in
batches, propagates errors and then reuses the existing scalar calculation over
cached child values through expr_default_eval_batch_func. The standalone scalar
path and JSON parsing/path semantics remain unchanged.

The actual production translation unit compiled successfully with native Clang 17
Release flags. Independent review and new-binary SQL GREEN are still required;
this development commit does not claim successful integration or platform results.
