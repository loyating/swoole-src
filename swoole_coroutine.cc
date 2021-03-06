/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Xinyu Zhu  <xyzhu1120@gmail.com>                             |
  |         shiguangqi <shiguangqi2008@gmail.com>                        |
  +----------------------------------------------------------------------+
 */

#include "php_swoole.h"
#include "swoole_coroutine.h"

using namespace swoole;

#define PHP_CORO_TASK_SLOT ((int)((ZEND_MM_ALIGNED_SIZE(sizeof(php_coro_task)) + ZEND_MM_ALIGNED_SIZE(sizeof(zval)) - 1) / ZEND_MM_ALIGNED_SIZE(sizeof(zval))))

bool PHPCoroutine::active = false;
uint64_t PHPCoroutine::max_num = SW_DEFAULT_MAX_CORO_NUM;
double PHPCoroutine::socket_connect_timeout = SW_DEFAULT_SOCKET_CONNECT_TIMEOUT;
double PHPCoroutine::socket_timeout = SW_DEFAULT_SOCKET_TIMEOUT;
php_coro_task PHPCoroutine::main_task = {0};

inline void PHPCoroutine::vm_stack_init(void)
{
    uint32_t size = SW_DEFAULT_PHP_STACK_PAGE_SIZE;
    zend_vm_stack page = (zend_vm_stack) emalloc(size);

    page->top = ZEND_VM_STACK_ELEMENTS(page);
    page->end = (zval*) ((char*) page + size);
    page->prev = NULL;

    EG(vm_stack) = page;
    EG(vm_stack)->top++;
    EG(vm_stack_top) = EG(vm_stack)->top;
    EG(vm_stack_end) = EG(vm_stack)->end;
#if PHP_VERSION_ID >= 70300
    EG(vm_stack_page_size) = size;
#endif
}

inline void PHPCoroutine::vm_stack_destroy(void)
{
    zend_vm_stack stack = EG(vm_stack);

    while (stack != NULL)
    {
        zend_vm_stack p = stack->prev;
        efree(stack);
        stack = p;
    }
}

/**
 * The meaning of the task argument in coro switch functions
 *
 * create: origin_task
 * yield: current_task
 * resume: target_task
 * close: current_task
 *
 */
inline void PHPCoroutine::save_vm_stack(php_coro_task *task)
{
#ifdef SW_CORO_SWAP_BAILOUT
    task->bailout = EG(bailout);
#endif
    task->vm_stack_top = EG(vm_stack_top);
    task->vm_stack_end = EG(vm_stack_end);
    task->vm_stack = EG(vm_stack);
#if PHP_VERSION_ID >= 70300
    task->vm_stack_page_size = EG(vm_stack_page_size);
#endif
    task->execute_data = EG(current_execute_data);
    task->error_handling = EG(error_handling);
    task->exception_class = EG(exception_class);
    task->exception = EG(exception);
    SW_SAVE_EG_SCOPE(task->scope);
}

inline void PHPCoroutine::restore_vm_stack(php_coro_task *task)
{
#ifdef SW_CORO_SWAP_BAILOUT
    EG(bailout) = task->bailout;
#endif
    EG(vm_stack_top) = task->vm_stack_top;
    EG(vm_stack_end) = task->vm_stack_end;
    EG(vm_stack) = task->vm_stack;
#if PHP_VERSION_ID >= 70300
    EG(vm_stack_page_size) = task->vm_stack_page_size;
#endif
    EG(current_execute_data) = task->execute_data;
    EG(error_handling) = task->error_handling;
    EG(exception_class) = task->exception_class;
    EG(exception) = task->exception;
    SW_SET_EG_SCOPE(task->scope);
}

inline void PHPCoroutine::save_og(php_coro_task *task)
{
    if (OG(handlers).elements)
    {
        task->output_ptr = (zend_output_globals *) emalloc(sizeof(zend_output_globals));
        memcpy(task->output_ptr, SWOG, sizeof(zend_output_globals));
        php_output_activate();
    }
    else
    {
        task->output_ptr = NULL;
    }
}

inline void PHPCoroutine::restore_og(php_coro_task *task)
{
    if (task->output_ptr)
    {
        memcpy(SWOG, task->output_ptr, sizeof(zend_output_globals));
        efree(task->output_ptr);
        task->output_ptr = NULL;
    }
}

sw_inline php_coro_task* PHPCoroutine::get_current_task()
{
    php_coro_task *task = (php_coro_task *) Coroutine::get_current_task();
    if (!task)
    {
        task = &main_task;
    }
    return task;
}

/**
 * create & resume ^ close
 */
sw_inline php_coro_task* PHPCoroutine::get_and_save_current_task()
{
    php_coro_task *task = get_current_task();
    save_vm_stack(task);
    save_og(task);
    return task;
}

void PHPCoroutine::on_yield(void *arg)
{
    php_coro_task *task = (php_coro_task *) arg;
    swTraceLog(SW_TRACE_COROUTINE,"php_coro_yield from cid=%ld to cid=%ld", Coroutine::get_cid(task->co), Coroutine::get_cid(task->origin_task->co));
    save_vm_stack(task);
    save_og(task);
    restore_vm_stack(task->origin_task);
    restore_og(task->origin_task);
}

void PHPCoroutine::on_resume(void *arg)
{
    php_coro_task *task = (php_coro_task *) arg;
    task->origin_task = get_and_save_current_task();
    restore_vm_stack(task);
    restore_og(task);
    swTraceLog(SW_TRACE_COROUTINE,"php_coro_resume from cid=%ld to cid=%ld", Coroutine::get_cid(task->origin_task->co), Coroutine::get_cid(task->co));
}

void PHPCoroutine::on_close(void *arg)
{
    php_coro_task *task = (php_coro_task *) arg;
    php_coro_task *origin_task = task->origin_task;
#ifdef SW_LOG_TRACE_OPEN
    long cid = Coroutine::get_cid(task->co);
    long origin_cid = Coroutine::get_cid(task->origin_task->co);
#endif

    if (SwooleG.hooks[SW_GLOBAL_HOOK_ON_CORO_STOP])
    {
        swoole_call_hook(SW_GLOBAL_HOOK_ON_CORO_STOP, task);
    }

    if (OG(handlers).elements)
    {
        if (OG(active))
        {
            php_output_end_all();
        }
        php_output_deactivate();
        php_output_activate();
    }
    vm_stack_destroy();
    restore_og(origin_task);
    restore_vm_stack(origin_task);

    swTraceLog(
        SW_TRACE_COROUTINE, "coro close cid=%ld and resume to %ld, %zu remained. usage size: %zu. malloc size: %zu",
        cid, origin_cid, (uintmax_t) Coroutine::count() - 1, (uintmax_t) zend_memory_usage(0), (uintmax_t) zend_memory_usage(1)
    );
}

void PHPCoroutine::create_func(void *arg)
{
    int i;
    php_coro_args *php_arg = (php_coro_args *) arg;
    zend_fcall_info_cache fci_cache = *php_arg->fci_cache;
    zend_function *func = fci_cache.function_handler;
    zval *argv = php_arg->argv;
    int argc = php_arg->argc;
    php_coro_task *task;
    php_coro_task *origin_task = php_arg->origin_task;
    zend_execute_data *call;
    zval _retval, *retval = &_retval;

    if (fci_cache.object)
    {
        GC_ADDREF(fci_cache.object);
    }

    vm_stack_init();
    call = (zend_execute_data *) (EG(vm_stack_top));
    task = (php_coro_task *) EG(vm_stack_top);
    EG(vm_stack_top) = (zval *) ((char *) call + PHP_CORO_TASK_SLOT * sizeof(zval));
    call = zend_vm_stack_push_call_frame(
        ZEND_CALL_TOP_FUNCTION | ZEND_CALL_ALLOCATED,
        func, argc, fci_cache.called_scope, fci_cache.object
    );

    SW_SET_EG_SCOPE(func->common.scope);

    for (i = 0; i < argc; ++i)
    {
        zval *param;
        zval *arg = &argv[i];
        if (Z_ISREF_P(arg) && !(func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE))
        {
            /* don't separate references for __call */
            arg = Z_REFVAL_P(arg);
        }
        param = ZEND_CALL_ARG(call, i + 1);
        ZVAL_COPY(param, arg);
    }

    call->symbol_table = NULL;

    if (func->op_array.fn_flags & ZEND_ACC_CLOSURE)
    {
        uint32_t call_info;
        GC_ADDREF(ZEND_CLOSURE_OBJECT(func));
        call_info = ZEND_CALL_CLOSURE;
        ZEND_ADD_CALL_FLAG(call, call_info);
    }

#ifdef SW_CORO_SWAP_BAILOUT
    EG(bailout) = NULL;
#endif
    EG(current_execute_data) = call;
    EG(error_handling) = EH_NORMAL;
    EG(exception_class) = NULL;
    EG(exception) = NULL;

    save_vm_stack(task);
    task->output_ptr = NULL;
    task->co = Coroutine::get_current();
    task->co->set_task((void *) task);
    task->defer_tasks = nullptr;
    task->origin_task = origin_task;
    task->pcid = Coroutine::get_cid(origin_task->co);

    swTraceLog(
        SW_TRACE_COROUTINE, "Create coro id: %ld, origin cid: %ld, coro total count: %zu, heap size: %zu",
        Coroutine::get_cid(task->co), Coroutine::get_cid(task->origin_task->co), (uintmax_t) Coroutine::count(), (uintmax_t) zend_memory_usage(0)
    );

    if (SwooleG.hooks[SW_GLOBAL_HOOK_ON_CORO_START])
    {
        swoole_call_hook(SW_GLOBAL_HOOK_ON_CORO_START, task);
    }

    if (EXPECTED(func->type == ZEND_USER_FUNCTION))
    {
        ZVAL_UNDEF(retval);
        // TODO: enhancement it, separate execute data is necessary, but we lose the backtrace
        EG(current_execute_data) = NULL;
#if PHP_VERSION_ID >= 70200
        zend_init_func_execute_data(call, &func->op_array, retval);
#else
        zend_init_execute_data(call, &func->op_array, retval);
#endif
        zend_execute_ex(EG(current_execute_data));
    }
    else /* ZEND_INTERNAL_FUNCTION */
    {
        ZVAL_NULL(retval);
        call->prev_execute_data = NULL;
        call->return_value = NULL; /* this is not a constructor call */
        execute_internal(call, retval);
        zend_vm_stack_free_args(call);
    }

    if (task->defer_tasks)
    {
        std::stack<defer_task *> *tasks = task->defer_tasks;
        while (!tasks->empty())
        {
            defer_task *task = tasks->top();
            tasks->pop();
            task->callback(task->data);
            delete task;
        }
        delete task->defer_tasks;
        task->defer_tasks = nullptr;
    }

    zval_ptr_dtor(retval);

    if (fci_cache.object)
    {
        OBJ_RELEASE(fci_cache.object);
    }

    if (UNEXPECTED(EG(exception)))
    {
        zend_exception_error(EG(exception), E_ERROR);
    }
}

long PHPCoroutine::create(zend_fcall_info_cache *fci_cache, uint32_t argc, zval *argv)
{
    zend_uchar type;
    if (unlikely(!active))
    {
        if (zend_get_module_started("xdebug") == SUCCESS)
        {
            swoole_php_fatal_error(E_WARNING, "Using Xdebug in coroutines is extremely dangerous, please notice that it may lead to coredump!");
        }
        php_swoole_check_reactor();
        // PHPCoroutine::enable_hook(SW_HOOK_ALL); // TODO: enable it in version 4.3.0
        active = true;
    }
    if (unlikely(Coroutine::count() >= max_num))
    {
        swoole_php_fatal_error(E_WARNING, "exceed max number of coroutine %zu.", (uintmax_t) Coroutine::count());
        return SW_CORO_ERR_LIMIT;
    }
    if (unlikely(!fci_cache || !fci_cache->function_handler))
    {
        swoole_php_fatal_error(E_ERROR, "invalid function call info cache.");
        return SW_CORO_ERR_INVALID;
    }
    type = fci_cache->function_handler->type;
    if (unlikely(type != ZEND_USER_FUNCTION && type != ZEND_INTERNAL_FUNCTION))
    {
        swoole_php_fatal_error(E_ERROR, "invalid function type %u.", fci_cache->function_handler->type);
        return SW_CORO_ERR_INVALID;
    }

    php_coro_args php_coro_args;
    php_coro_args.fci_cache = fci_cache;
    php_coro_args.argv = argv;
    php_coro_args.argc = argc;
    php_coro_args.origin_task = get_and_save_current_task();

    return Coroutine::create(create_func, (void*) &php_coro_args);
}

void PHPCoroutine::defer(swCallback cb, void *data)
{
    php_coro_task *task = get_current_task();
    if (task->defer_tasks == nullptr)
    {
        task->defer_tasks = new std::stack<defer_task *>;
    }
    task->defer_tasks->push(new defer_task(cb, data));
}

void PHPCoroutine::check()
{
    if (unlikely(!is_in()))
    {
        swoole_php_fatal_error(E_ERROR, "must be called in the coroutine.");
    }
}

void PHPCoroutine::check_bind(const char *name, long bind_cid)
{
    check();
    if (unlikely(bind_cid > 0))
    {
        swString *buffer = SwooleTG.buffer_stack;
        sw_get_debug_print_backtrace(buffer, DEBUG_BACKTRACE_IGNORE_ARGS, 3);
        swoole_error_log(
            SW_LOG_ERROR, SW_ERROR_CO_HAS_BEEN_BOUND,
            "%s has already been bound to another coroutine#%ld, "
            "reading or writing of the same socket in multiple coroutines at the same time is not allowed.\n"
            "%.*s", name, bind_cid, (int) buffer->length, buffer->str
        );
        exit(255);
    }
}

void PHPCoroutine::yield_m(zval *return_value, php_coro_context *sw_current_context)
{
    if (unlikely(!is_in()))
    {
        swoole_php_fatal_error(E_ERROR, "must be called in the coroutine.");
    }
    php_coro_task *task = get_current_task();
    sw_current_context->current_coro_return_value_ptr = return_value;
    sw_current_context->current_task = task;
    on_yield(task);
    task->co->yield_naked();
}

int PHPCoroutine::resume_m(php_coro_context *sw_current_context, zval *retval, zval *coro_retval)
{
    php_coro_task *task = sw_current_context->current_task;
    on_resume(task);
    if (EG(current_execute_data)->prev_execute_data->opline->result_type != IS_UNUSED && retval)
    {
        ZVAL_COPY(sw_current_context->current_coro_return_value_ptr, retval);
    }
    task->co->resume_naked();
    return SW_CORO_ERR_END;
}
