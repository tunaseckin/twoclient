use freya::prelude::*;

use crate::components::{ActiveClusterPanel, RecentsRow};
use crate::theme;

#[derive(PartialEq)]
pub struct Home;

impl Component for Home {
    fn render(&self) -> impl IntoElement {
        if *crate::edition::use_edition().current.read() == crate::edition::Edition::Bedrock {
            return crate::view::app::BedrockHome.into_element();
        }
        rect()
            .vertical()
            .width(Size::fill())
            .height(Size::fill())
            .content(Content::Flex)
            .overflow(Overflow::Clip)
            .padding(theme::HOME_PADDING_PX)
            .main_align(Alignment::Center)
            .child(
                rect()
                    .vertical()
                    .width(Size::fill())
                    .height(Size::flex(1.0))
                    .main_align(Alignment::Center)
                    .child(ActiveClusterPanel),
            )
            .child(RecentsRow)
            .into_element()
    }
}
