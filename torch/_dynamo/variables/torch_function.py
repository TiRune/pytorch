import inspect
from typing import Dict, List

import torch.utils._pytree as pytree

from torch.overrides import _get_overloaded_args, get_default_nowrap_functions
from ..exc import unimplemented
from ..source import AttrSource, GlobalSource
from ..utils import is_tensor_base_attr_getter
from .base import VariableTracker
from .constant import ConstantVariable
from .lists import TupleVariable
from .tensor import TensorVariable
from .user_defined import UserDefinedClassVariable


# [Note: __torch_function__] This feature is a prototype and has some rough edges (contact mlazos with issues):
# At a high level, a torch function tensor subclass is represented as a TensorWithTFOverrideVariable, which dispatches
# __torch_function__ on attribute accesses, method calls, and torch API calls.
# The following is not supported:
# - triggering __torch_function__ on tensor subclass non-tensor custom attributes
# - graph breaking on mutating guardable tensor properties within a __torch_function__ context, this can cause
# excessive recompiles in certain degenerate cases
# - Matching the exact eager behavior of *ignoring* __torch_function__ objects in non-tensor argument positions of Torch API calls

# The following is supported:
# - static method impls of __torch_function__ on custom objects; this will trigger on torch API calls with the object as
# any argument
# - triggering __torch_function__ on torch API calls with tensor subclass arguments
# - __torch_function__ calls on base tensor attribute access and method calls for tensor subclass instances
# - matches the dispatch ordering behavior of eager __torch_function__ with subclass/object argumnents in any argument position

# See https://docs.google.com/document/d/1WBxBSvW3NXhRp9ncmtokJloMLCtF4AYNhJaffvHe8Kw/edit#heading=h.vacn73lozd9w
# for more information on the design.

# To enable subclass behavior, add your tensor subclass type to traceable_tensor_subclasses in dynamo/config.py


banned_attrs = [
    fn.__self__.__name__
    for fn in get_default_nowrap_functions()
    if is_tensor_base_attr_getter(fn)
]


def is_torch_function_user_object(obj):
    return hasattr(obj, "__torch_function__")


def _is_attr_overidden(tx, var, name):
    import torch

    overridden = False
    try:
        attr_val = inspect.getattr_static(var.python_type(), name)
        overridden |= attr_val != getattr(torch.Tensor, name)
    except AttributeError:
        pass

    return overridden


def call_torch_function(
    tx, torch_function_type, torch_function_var, fn, types, args, kwargs
):
    # signature:
    # def __torch_function__(cls, func, types, args=(), kwargs=None):
    tf_args = (torch_function_type, fn, types, TupleVariable(list(args)))
    return tx.inline_user_function_return(torch_function_var, tf_args, kwargs)


def build_torch_function_fn(tx, value, source):
    from .builder import SourcelessBuilder, VariableBuilder

    if not source:
        return VariableBuilder(
            tx,
            AttrSource(AttrSource(source, "__torch_function__"), "__func__"),
        )(value.__torch_function__.__func__)
    else:
        return SourcelessBuilder()(tx, value.__torch_function__.__func__)


def can_dispatch_torch_function(tx, args, kwargs):
    if tx.output.torch_function_enabled:
        all_args = pytree.tree_leaves(args) + pytree.tree_leaves(kwargs)
        return any(isinstance(arg, TensorWithTFOverrideVariable) for arg in all_args)
    else:
        return False


def dispatch_torch_function(tx, fn, args, kwargs):
    """Gathers all args that are TensorWithTFOverrideVariable and dispatches based on the ordering in _get_overloaded_args"""

    all_args = pytree.tree_leaves(args) + pytree.tree_leaves(kwargs)
    overloaded_args = _get_overloaded_args(
        [arg for arg in all_args if isinstance(arg, TensorWithTFOverrideVariable)],
        lambda x: x.class_type,
    )

    for arg in overloaded_args:
        res = arg.call_torch_function(
            tx,
            fn,
            TupleVariable([arg.subclass_type_var() for arg in overloaded_args]),
            args,
            kwargs,
        )

        if not (isinstance(res, ConstantVariable) and res.value is NotImplemented):
            return res

    unimplemented(
        f"All __torch_function__ overrides for call {fn} with args {args} and kwargs {kwargs} returned NotImplemented"
    )


class TensorWithTFOverrideVariable(TensorVariable):
    """
    Represents a tensor subclass instance with a __torch_function__ override.
    """

    def __init__(self, *args, **kwargs):
        self.torch_function_fn = kwargs.pop("torch_function_fn")
        super().__init__(*args, **kwargs)

    @classmethod
    def from_tensor_var(cls, tx, tensor_var, class_type, torch_function_fn):
        import torch

        kwargs = dict(tensor_var.__dict__)
        assert (
            kwargs.pop("class_type") is torch.Tensor
        ), "invalid class type in TensorWithTFOverrideVariable.from_tensor_var"
        var = cls(torch_function_fn=torch_function_fn, class_type=class_type, **kwargs)

        # stash the subclass type to rewrap an output tensor if needed
        # this is needed because the actual type needs to be available
        # each time the compiled artifact is run and outputs a wrapped tensor.
        if var.global_mangled_class_name() not in tx.output.global_scope:
            tx.output.install_global(var.global_mangled_class_name(), class_type)

        return var

    def python_type(self):
        return self.class_type

    def subclass_type_var(self):
        return UserDefinedClassVariable(
            self.class_type, source=GlobalSource(self.global_mangled_class_name())
        )

    def global_mangled_class_name(self):
        return f"__subclass_{self.class_type.__name__}_{id(self.class_type)}"

    def var_getattr(self, tx, name):
        # [Note: __torch_function__] We currently only support attributes that are defined on
        # base tensors, custom attribute accesses will graph break.
        import torch
        from .builder import SourcelessBuilder, VariableBuilder

        if name in banned_attrs or not hasattr(torch.Tensor, name):
            unimplemented(
                f"Accessing {name} on a tensor subclass with a __torch_function__ override is not supported"
            )

        if _is_attr_overidden(tx, self, name):
            unimplemented(
                f"Accessing overidden method/attribute {name} on a tensor"
                " subclass with a __torch_function__ override is not supported"
            )

        if tx.output.torch_function_enabled:
            if self.source:
                get_fn = VariableBuilder(
                    tx,
                    source=AttrSource(
                        AttrSource(AttrSource(self.source, "__class__"), name),
                        "__get__",
                    ),
                )(inspect.getattr_static(self.python_type(), name).__get__)
            else:
                get_fn = SourcelessBuilder()(tx, getattr(torch.Tensor, name).__get__)

            return self.call_torch_function(
                tx,
                get_fn,
                TupleVariable([self.subclass_type_var()]),
                [self],
                {},
            )
        else:
            return super().var_getattr(tx, name)

    def call_torch_function(self, tx, fn, types, args, kwargs):
        return call_torch_function(
            tx,
            self.subclass_type_var(),
            self.torch_function_fn,
            fn,
            types,
            args,
            kwargs,
        )

    def call_method(
        self,
        tx,
        name,
        args: "List[VariableTracker]",
        kwargs: "Dict[str, VariableTracker]",
    ) -> "VariableTracker":
        # This code block implements inlining the __torch_function__ override
        # of `call_method`.
        if tx.output.torch_function_enabled:
            import torch
            from .builder import SourcelessBuilder, VariableBuilder

            if _is_attr_overidden(tx, self, name):
                unimplemented(
                    f"Calling overidden method {name} on a tensor"
                    " subclass with a __torch_function__ override is not supported"
                )

            # [Note: __torch_function__] Currently we only support methods that are defined on tensor
            # we will graph break in other cases this will need a bigger overhaul of extracting methods/comparing them for equality
            # We've established with the above check that the method is not overridden, so we guard that the method is the same
            # as the impl defined on tensor and retrieve it
            if self.source:
                func_var = VariableBuilder(
                    tx, AttrSource(AttrSource(self.source, "__class__"), name)
                )(inspect.getattr_static(self.python_type(), name))
            else:
                func_var = SourcelessBuilder()(tx, getattr(torch.Tensor, name))
            return dispatch_torch_function(tx, func_var, [self] + args, kwargs)
        else:
            return self.tensor_variable.call_method(tx, name, args, kwargs)
