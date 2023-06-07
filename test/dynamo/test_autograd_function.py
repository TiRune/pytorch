# Owner(s): ["module: dynamo"]

import itertools
import math

import torch

import torch._dynamo.test_case
import torch._dynamo.testing


class CustomFunc1(torch.autograd.Function):
    @staticmethod
    def forward(ctx, foo):
        return foo + foo

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output


class CustomFunc3(torch.autograd.Function):
    # Test there is graph break in forward function
    @staticmethod
    def forward(ctx, foo):
        result = foo + foo
        torch._dynamo.graph_break()
        result = result + foo
        ctx.save_for_backward(result)
        return result

    @staticmethod
    def backward(ctx, grad_output):
        (result,) = ctx.saved_tensors
        return grad_output * math.sqrt(result.numel())


class Module1(torch.nn.Module):
    def forward(self, foo):
        return CustomFunc1().apply(foo)


class Module2(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.fn = CustomFunc1.apply

    def forward(self, foo):
        return self.fn(foo)


class Module3(torch.nn.Module):
    def forward(self, foo):
        return CustomFunc1().apply(foo)


class Module4(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.fn = CustomFunc1.apply

    def forward(self, foo):
        return self.fn(foo)


class Module5(torch.nn.Module):
    def forward(self, foo):
        return CustomFunc3().apply(foo)


class Module6(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.fn = CustomFunc3.apply

    def forward(self, foo):
        return self.fn(foo)


class LinearFunction(torch.autograd.Function):
    # Note that forward, setup_context, and backward are @staticmethods
    @staticmethod
    def forward(input, weight, bias):
        output = input.mm(weight.t())
        if bias is not None:
            output += bias.unsqueeze(0).expand_as(output)
        return output

    @staticmethod
    # inputs is a Tuple of all of the inputs passed to forward.
    # output is the output of the forward().
    def setup_context(ctx, inputs, output):
        input, weight, bias = inputs
        ctx.save_for_backward(input, weight, bias)

    # This function has only a single output, so it gets only one gradient
    @staticmethod
    def backward(ctx, grad_output):
        input, weight, bias = ctx.saved_tensors
        grad_input = grad_weight = grad_bias = None
        if ctx.needs_input_grad[0]:
            grad_input = grad_output.mm(weight)
        if ctx.needs_input_grad[1]:
            grad_weight = grad_output.t().mm(input)
        if bias is not None and ctx.needs_input_grad[2]:
            grad_bias = grad_output.sum(0)

        return grad_input, grad_weight, grad_bias


class ModuleLinear(torch.nn.Module):
    def forward(self, input, weight, bias=None):
        return LinearFunction.apply(input, weight, bias)


class MaterializingGradFunction(torch.autograd.Function):
    @staticmethod
    def forward(ctx, x):
        ctx.set_materialize_grads(False)
        return x.clone(), x.clone()

    @staticmethod
    def backward(ctx, grad_out1, grad_out2):
        return grad_out1, grad_out2


class MaterializingGradModule(torch.nn.Module):
    def forward(self, x):
        return MaterializingGradFunction.apply(x)


class CustomFuncBwdPrintGraphBreak(torch.autograd.Function):
    @staticmethod
    def forward(ctx, foo):
        return torch.add(foo, foo)

    @staticmethod
    def backward(ctx, grad_output):
        print("graph break!")
        return grad_output


class CustomFuncBwdPrintModule(torch.nn.Module):
    def forward(self, x):
        return CustomFuncBwdPrintGraphBreak.apply(x)


class CustomFuncStrideBwd(torch.autograd.Function):
    @staticmethod
    def forward(ctx, foo):
        return torch.add(foo, foo)

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output.stride()


class CustomFuncStrideModule(torch.nn.Module):
    def forward(self, x):
        return CustomFuncStrideBwd.apply(x)


class CustomFuncSaveForBwd(torch.autograd.Function):
    @staticmethod
    def forward(ctx, foo):
        result = foo + foo
        result = result + foo
        ctx.save_for_backward(result)
        return result

    @staticmethod
    def backward(ctx, grad_output):
        (result,) = ctx.saved_tensors
        return grad_output * math.sqrt(result.numel())


class SaveForBwdModule(torch.nn.Module):
    def forward(self, foo):
        return CustomFuncSaveForBwd().apply(foo)


class AutogradFunctionTests(torch._dynamo.test_case.TestCase):
    # Sound behaviors, tested for working capture
    def test_autograd_function_equivalence(self):
        for grad in [True, False]:
            for i in range(1, 5):
                torch._dynamo.reset()
                model = globals()[f"Module{i}"]()
                opt_model = torch._dynamo.optimize("eager")(model)
                self.assertTrue(
                    torch.allclose(
                        opt_model(torch.ones(2, 3, requires_grad=grad)),
                        torch.tensor([2.0], requires_grad=grad),
                    )
                )

    def test_autograd_function_has_graph_break(self):
        for grad in [True, False]:
            x = torch.randn(10, requires_grad=grad)
            for model in [Module5(), Module6()]:
                torch._dynamo.reset()
                cnts = torch._dynamo.testing.CompileCounter()
                opt_model = torch._dynamo.optimize(cnts)(model)
                for _ in range(3):
                    ref = model(x)
                    res = opt_model(x)
                    self.assertTrue(torch.allclose(ref, res))
                self.assertEqual(cnts.frame_count, 2)

    def test_linear_setup_context(self):
        model = ModuleLinear()
        opt_model = torch._dynamo.optimize("eager")(model)
        input = torch.randn(2, 2, dtype=torch.double, requires_grad=True)
        weight = torch.randn(3, 2, dtype=torch.double, requires_grad=True)
        optim_result = opt_model(input, weight)
        eager_result = model(input, weight)
        self.assertEqual(optim_result, eager_result)

    def test_materialize_grad(self):
        model = MaterializingGradModule()
        opt_model = torch._dynamo.optimize("eager")(model)
        x = torch.randn(2, 2, dtype=torch.double, requires_grad=True)
        optim_result = opt_model(x)
        eager_result = model(x)
        self.assertEqual(optim_result, eager_result)

    def test_print_in_bwd(self):
        model = CustomFuncBwdPrintModule()
        opt_model = torch._dynamo.optimize("eager", nopython=True)(model)
        x = torch.randn(2, 2, dtype=torch.double, requires_grad=True)
        with self.assertRaisesRegex(
            torch._dynamo.exc.Unsupported, ".*BuiltinVariable\\(print\\).*"
        ):
            opt_model(x)

    def test_stride_in_bwd(self):
        model = CustomFuncStrideModule()
        opt_model = torch._dynamo.optimize("eager", nopython=True)(model)
        x = torch.randn(2, 2, dtype=torch.double, requires_grad=True)
        with self.assertRaisesRegex(
            torch._dynamo.exc.Unsupported,
            "Illegal getattr invocation stride in strict mod",
        ):
            opt_model(x)

    def test_save_for_bwd(self):
        model = SaveForBwdModule()
        opt_model = torch._dynamo.optimize("eager", nopython=True)(model)
        x = torch.randn(2, 2, dtype=torch.double, requires_grad=True)
        opt_model(x)

    def test_classmethod(self):
        def conv3x3(in_planes, out_planes, stride=1):
            """3x3 convolution with padding"""
            return torch.nn.Conv2d(
                in_planes,
                out_planes,
                kernel_size=3,
                stride=stride,
                padding=1,
                bias=False,
            )

        class Shake(torch.autograd.Function):
            @classmethod
            def forward(cls, ctx, inp1, inp2, training):
                assert inp1.size() == inp2.size()
                gate_size = [inp1.size()[0], *itertools.repeat(1, inp1.dim() - 1)]
                gate = inp1.new(*gate_size)
                if training:
                    gate.uniform_(0, 1)
                else:
                    gate.fill_(0.5)
                return inp1 * gate + inp2 * (1.0 - gate)

            @classmethod
            def backward(cls, ctx, grad_output):
                grad_inp1 = grad_inp2 = grad_training = None
                gate_size = [
                    grad_output.size()[0],
                    *itertools.repeat(1, grad_output.dim() - 1),
                ]
                gate = grad_output.data.new(*gate_size).uniform_(0, 1)
                if ctx.needs_input_grad[0]:
                    grad_inp1 = grad_output * gate
                if ctx.needs_input_grad[1]:
                    grad_inp2 = grad_output * (1 - gate)
                assert not ctx.needs_input_grad[2]
                return grad_inp1, grad_inp2, grad_training

        def shake(inp1, inp2, training=False):
            return Shake.apply(inp1, inp2, training)

        class ShakeShakeBlock(torch.nn.Module):
            @classmethod
            def out_channels(cls, planes, groups):
                assert groups == 1
                return planes

            def __init__(
                self, inplanes=4, planes=4, groups=1, stride=1, downsample=None
            ):
                super().__init__()
                assert groups == 1
                self.conv_a1 = conv3x3(inplanes, planes, stride)
                self.conv_b1 = conv3x3(inplanes, planes, stride)

            def forward(self, x):
                a, b = x, x
                a = self.conv_a1(a)
                b = self.conv_b1(b)
                ab = shake(a, b, training=self.training)
                return ab

        x = torch.rand(4, 4, 4, 4)
        m = ShakeShakeBlock()
        opt_m = torch.compile(backend="eager")(m)
        opt_m(x)


if __name__ == "__main__":
    from torch._dynamo.test_case import run_tests

    run_tests()
